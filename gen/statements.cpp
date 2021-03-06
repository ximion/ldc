//===-- statements.cpp ----------------------------------------------------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#include "init.h"
#include "mars.h"
#include "module.h"
#include "mtype.h"
#include "port.h"
#include "gen/abi.h"
#include "gen/arrays.h"
#include "gen/classes.h"
#include "gen/coverage.h"
#include "gen/dvalue.h"
#include "gen/irstate.h"
#include "gen/llvm.h"
#include "gen/llvmhelpers.h"
#include "gen/logger.h"
#include "gen/runtime.h"
#include "gen/tollvm.h"
#include "gen/ms-cxx-helper.h"
#include "ir/irfunction.h"
#include "ir/irmodule.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InlineAsm.h"
#include <fstream>
#include <math.h>
#include <stdio.h>

// Need to include this after the other DMD includes because of missing
// dependencies.
#include "hdrgen.h"

//////////////////////////////////////////////////////////////////////////////
// FIXME: Integrate these functions
void AsmStatement_toIR(AsmStatement *stmt, IRState *irs);
void CompoundAsmStatement_toIR(CompoundAsmStatement *stmt, IRState *p);

//////////////////////////////////////////////////////////////////////////////

// used to build the sorted list of cases
struct Case {
  StringExp *str;
  size_t index;

  Case(StringExp *s, size_t i) {
    str = s;
    index = i;
  }

  friend bool operator<(const Case &l, const Case &r) {
    return l.str->compare(r.str) < 0;
  }
};

static LLValue *call_string_switch_runtime(llvm::Value *table, Expression *e) {
  Type *dt = e->type->toBasetype();
  Type *dtnext = dt->nextOf()->toBasetype();
  TY ty = dtnext->ty;
  const char *fname;
  if (ty == Tchar) {
    fname = "_d_switch_string";
  } else if (ty == Twchar) {
    fname = "_d_switch_ustring";
  } else if (ty == Tdchar) {
    fname = "_d_switch_dstring";
  } else {
    llvm_unreachable("not char/wchar/dchar");
  }

  llvm::Function *fn = getRuntimeFunction(e->loc, gIR->module, fname);

  IF_LOG {
    Logger::cout() << *table->getType() << '\n';
    Logger::cout() << *fn->getFunctionType()->getParamType(0) << '\n';
  }
  assert(table->getType() == fn->getFunctionType()->getParamType(0));

  DValue *val = toElemDtor(e);
  LLValue *llval = DtoRVal(val);
  assert(llval->getType() == fn->getFunctionType()->getParamType(1));

  LLCallSite call = gIR->CreateCallOrInvoke(fn, table, llval);

  return call.getInstruction();
}

//////////////////////////////////////////////////////////////////////////////

class ToIRVisitor : public Visitor {
  IRState *irs;

public:
  explicit ToIRVisitor(IRState *irs) : irs(irs) {}

  //////////////////////////////////////////////////////////////////////////

  // Import all functions from class Visitor
  using Visitor::visit;

  //////////////////////////////////////////////////////////////////////////

  void visit(CompoundStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("CompoundStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    for (auto s : *stmt->statements) {
      if (s) {
        s->accept(this);
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ReturnStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ReturnStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    // The LLVM value to return, or null for void returns.
    LLValue *returnValue = nullptr;

    IrFunction *const f = irs->func();
    FuncDeclaration *const fd = f->decl;
    LLFunction *const llFunc = f->func;

    // is there a return value expression?
    if (stmt->exp || (!stmt->exp && (llFunc == irs->mainFunc))) {
      // if the function's return type is void, it uses sret
      if (llFunc->getReturnType() == LLType::getVoidTy(irs->context())) {
        assert(!f->type->isref);

        LLValue *sretPointer = getIrFunc(fd)->sretArg;
        assert(sretPointer);

        assert(!f->irFty.arg_sret->rewrite &&
               "ABI shouldn't have to rewrite sret returns");
        DLValue returnValue(f->type->next, sretPointer);

        // try to construct the return value in-place
        const auto initialCleanupScope = f->scopes->currentCleanupScope();
        const bool constructed = toInPlaceConstruction(&returnValue, stmt->exp);
        if (constructed) {
          // cleanup manually (otherwise done by toElemDtor())
          if (f->scopes->currentCleanupScope() != initialCleanupScope) {
            auto endbb = llvm::BasicBlock::Create(
                irs->context(), "inPlaceSretConstruct.success", llFunc);
            f->scopes->runCleanups(initialCleanupScope, endbb);
            f->scopes->popCleanups(initialCleanupScope);
            irs->scope() = IRScope(endbb);
          }
        } else {
          DValue *e = toElemDtor(stmt->exp);

          // store the return value unless NRVO already used the sret pointer
          if (!e->isLVal() || DtoLVal(e) != sretPointer) {
            // call postblit if the expression is a D lvalue
            // exceptions: NRVO and special __result variable (out contracts)
            bool doPostblit = !(fd->nrvo_can && fd->nrvo_var);
            if (doPostblit && stmt->exp->op == TOKvar) {
              auto ve = static_cast<VarExp *>(stmt->exp);
              if (ve->var->isResult())
                doPostblit = false;
            }

            DtoAssign(stmt->loc, &returnValue, e, TOKblit);
            if (doPostblit)
              callPostblit(stmt->loc, stmt->exp, sretPointer);
          }
        }
      }
      // the return type is not void, so this is a normal "register" return
      else {
        if (!stmt->exp && (llFunc == irs->mainFunc)) {
          returnValue =
              LLConstant::getNullValue(irs->mainFunc->getReturnType());
        } else {
          if (stmt->exp->op == TOKnull) {
            stmt->exp->type = f->type->next;
          }
          DValue *dval = nullptr;
          // call postblit if necessary
          if (!f->type->isref) {
            dval = toElemDtor(stmt->exp);
            LLValue *vthis =
                (DtoIsInMemoryOnly(dval->type) ? DtoLVal(dval) : DtoRVal(dval));
            callPostblit(stmt->loc, stmt->exp, vthis);
          } else {
            Expression *ae = stmt->exp;
            dval = toElemDtor(ae);
          }
          // do abi specific transformations on the return value
          returnValue = getIrFunc(fd)->irFty.putRet(dval);
        }

        // Hack around LDC assuming structs and static arrays are in memory:
        // If the function returns a struct or a static array, and the return
        // value is a pointer to a struct or a static array, load from it
        // before returning.
        if (returnValue->getType() != llFunc->getReturnType() &&
            DtoIsInMemoryOnly(f->type->next) &&
            isaPointer(returnValue->getType())) {
          Logger::println("Loading value for return");
          returnValue = DtoLoad(returnValue);
        }

        // can happen for classes and void main
        if (returnValue->getType() != llFunc->getReturnType()) {
          // for the main function this only happens if it is declared as void
          // and then contains a return (exp); statement. Since the actual
          // return type remains i32, we just throw away the exp value
          // and return 0 instead
          // if we're not in main, just bitcast
          if (llFunc == irs->mainFunc) {
            returnValue =
                LLConstant::getNullValue(irs->mainFunc->getReturnType());
          } else {
            returnValue =
                irs->ir->CreateBitCast(returnValue, llFunc->getReturnType());
          }

          IF_LOG Logger::cout() << "return value after cast: " << *returnValue
                                << '\n';
        }
      }
    } else {
      // no return value expression means it's a void function.
      assert(llFunc->getReturnType() == LLType::getVoidTy(irs->context()));
    }

    // If there are no cleanups to run, we try to keep the IR simple and
    // just directly emit the return instruction. If there are cleanups to run
    // first, we need to store the return value to a stack slot, in which case
    // we can use a shared return bb for all these cases.
    const bool useRetValSlot = f->scopes->currentCleanupScope() != 0;
    const bool sharedRetBlockExists = !!f->retBlock;
    if (useRetValSlot) {
      if (!sharedRetBlockExists) {
        f->retBlock =
            llvm::BasicBlock::Create(irs->context(), "return", llFunc);
        if (returnValue) {
          f->retValSlot =
              DtoRawAlloca(returnValue->getType(), 0, "return.slot");
        }
      }

      // Create the store to the slot at the end of our current basic
      // block, before we run the cleanups.
      if (returnValue) {
        irs->ir->CreateStore(returnValue, f->retValSlot);
      }

      // Now run the cleanups.
      f->scopes->runAllCleanups(f->retBlock);

      irs->scope() = IRScope(f->retBlock);
    }

    // If we need to emit the actual return instruction, do so.
    if (!useRetValSlot || !sharedRetBlockExists) {
      if (returnValue) {
        // Hack: the frontend generates 'return 0;' as last statement of
        // 'void main()'. But the debug location is missing. Use the end
        // of function as debug location.
        if (fd->isMain() && !stmt->loc.linnum) {
          irs->DBuilder.EmitStopPoint(fd->endloc);
        }

        irs->ir->CreateRet(useRetValSlot ? DtoLoad(f->retValSlot)
                                         : returnValue);
      } else {
        irs->ir->CreateRetVoid();
      }
    }

    // Finally, create a new predecessor-less dummy bb as the current IRScope
    // to make sure we do not emit any extra instructions after the terminating
    // instruction (ret or branch to return bb), which would be illegal IR.
    irs->scope() = IRScope(
        llvm::BasicBlock::Create(gIR->context(), "dummy.afterreturn", llFunc));
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ExpStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ExpStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    if (stmt->exp) {
      elem *e;
      // a cast(void) around the expression is allowed, but doesn't require any
      // code
      if (stmt->exp->op == TOKcast && stmt->exp->type == Type::tvoid) {
        CastExp *cexp = static_cast<CastExp *>(stmt->exp);
        e = toElemDtor(cexp->e1);
      } else {
        e = toElemDtor(stmt->exp);
      }
      delete e;
    }
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(IfStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("IfStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);
    auto truecount = PGO.getRegionCount(stmt);
    auto elsecount = PGO.getCurrentRegionCount() - truecount;
    auto brweights = PGO.createProfileWeights(truecount, elsecount);

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);
    emitCoverageLinecountInc(stmt->loc);

    if (stmt->match) {
      DtoRawVarDeclaration(stmt->match);
    }

    DValue *cond_e = toElemDtor(stmt->condition);
    LLValue *cond_val = DtoRVal(cond_e);

    llvm::BasicBlock *ifbb =
        llvm::BasicBlock::Create(irs->context(), "if", irs->topfunc());
    llvm::BasicBlock *endbb =
        llvm::BasicBlock::Create(irs->context(), "endif", irs->topfunc());
    llvm::BasicBlock *elsebb =
        stmt->elsebody ? llvm::BasicBlock::Create(irs->context(), "else",
                                                  irs->topfunc(), endbb)
                       : endbb;

    if (cond_val->getType() != LLType::getInt1Ty(irs->context())) {
      IF_LOG Logger::cout() << "if conditional: " << *cond_val << '\n';
      cond_val = DtoRVal(DtoCast(stmt->loc, cond_e, Type::tbool));
    }
    auto brinstr =
        llvm::BranchInst::Create(ifbb, elsebb, cond_val, irs->scopebb());
    PGO.addBranchWeights(brinstr, brweights);

    // replace current scope
    irs->scope() = IRScope(ifbb);

    // do scoped statements

    if (stmt->ifbody) {
      irs->DBuilder.EmitBlockStart(stmt->ifbody->loc);
      PGO.emitCounterIncrement(stmt);
      stmt->ifbody->accept(this);
      irs->DBuilder.EmitBlockEnd();
    }
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(endbb, irs->scopebb());
    }

    if (stmt->elsebody) {
      irs->scope() = IRScope(elsebb);
      irs->DBuilder.EmitBlockStart(stmt->elsebody->loc);
      stmt->elsebody->accept(this);
      if (!irs->scopereturned()) {
        llvm::BranchInst::Create(endbb, irs->scopebb());
      }
      irs->DBuilder.EmitBlockEnd();
    }

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();

    // rewrite the scope
    irs->scope() = IRScope(endbb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ScopeStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ScopeStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    if (stmt->statement) {
      irs->DBuilder.EmitBlockStart(stmt->statement->loc);
      stmt->statement->accept(this);
      irs->DBuilder.EmitBlockEnd();
    }
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(WhileStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("WhileStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // create while blocks

    llvm::BasicBlock *whilebb =
        llvm::BasicBlock::Create(irs->context(), "whilecond", irs->topfunc());
    llvm::BasicBlock *whilebodybb =
        llvm::BasicBlock::Create(irs->context(), "whilebody", irs->topfunc());
    llvm::BasicBlock *endbb =
        llvm::BasicBlock::Create(irs->context(), "endwhile", irs->topfunc());

    // move into the while block
    irs->ir->CreateBr(whilebb);

    // replace current scope
    irs->scope() = IRScope(whilebb);

    // create the condition
    emitCoverageLinecountInc(stmt->condition->loc);
    DValue *cond_e = toElemDtor(stmt->condition);
    LLValue *cond_val = DtoRVal(DtoCast(stmt->loc, cond_e, Type::tbool));
    delete cond_e;

    // conditional branch
    auto branchinst =
        llvm::BranchInst::Create(whilebodybb, endbb, cond_val, irs->scopebb());
    {
      auto loopcount = PGO.getRegionCount(stmt);
      auto brweights =
          PGO.createProfileWeightsWhileLoop(stmt->condition, loopcount);
      PGO.addBranchWeights(branchinst, brweights);
    }

    // rewrite scope
    irs->scope() = IRScope(whilebodybb);

    // while body code
    irs->func()->scopes->pushLoopTarget(stmt, whilebb, endbb);
    PGO.emitCounterIncrement(stmt);
    if (stmt->_body) {
      stmt->_body->accept(this);
    }
    irs->func()->scopes->popLoopTarget();

    // loop
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(whilebb, irs->scopebb());
    }

    // rewrite the scope
    irs->scope() = IRScope(endbb);

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(DoStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("DoStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    auto entryCount = PGO.setCurrentStmt(stmt);

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // create while blocks
    llvm::BasicBlock *dowhilebb =
        llvm::BasicBlock::Create(irs->context(), "dowhile", irs->topfunc());
    llvm::BasicBlock *condbb =
        llvm::BasicBlock::Create(irs->context(), "dowhilecond", irs->topfunc());
    llvm::BasicBlock *endbb =
        llvm::BasicBlock::Create(irs->context(), "enddowhile", irs->topfunc());

    // move into the while block
    assert(!irs->scopereturned());
    llvm::BranchInst::Create(dowhilebb, irs->scopebb());

    // replace current scope
    irs->scope() = IRScope(dowhilebb);

    // do-while body code
    irs->func()->scopes->pushLoopTarget(stmt, condbb, endbb);
    PGO.emitCounterIncrement(stmt);
    if (stmt->_body) {
      stmt->_body->accept(this);
    }
    irs->func()->scopes->popLoopTarget();

    // branch to condition block
    llvm::BranchInst::Create(condbb, irs->scopebb());
    irs->scope() = IRScope(condbb);

    // create the condition
    emitCoverageLinecountInc(stmt->condition->loc);
    DValue *cond_e = toElemDtor(stmt->condition);
    LLValue *cond_val = DtoRVal(DtoCast(stmt->loc, cond_e, Type::tbool));
    delete cond_e;

    // conditional branch
    auto branchinst =
        llvm::BranchInst::Create(dowhilebb, endbb, cond_val, irs->scopebb());
    {
      // The region counter includes fallthrough from the previous statement.
      // Subtract parent count to get the true branch count of the loop
      // conditional.
      auto loopcount = PGO.getRegionCount(stmt) - entryCount;
      auto brweights =
          PGO.createProfileWeightsWhileLoop(stmt->condition, loopcount);
      PGO.addBranchWeights(branchinst, brweights);
    }

    // Order the blocks in a logical order in IR
    condbb->moveAfter(&irs->topfunc()->back());
    endbb->moveAfter(condbb);

    // rewrite the scope
    irs->scope() = IRScope(endbb);

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ForStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ForStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // start new dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // create for blocks
    llvm::BasicBlock *forbb =
        llvm::BasicBlock::Create(irs->context(), "forcond", irs->topfunc());
    llvm::BasicBlock *forbodybb =
        llvm::BasicBlock::Create(irs->context(), "forbody", irs->topfunc());
    llvm::BasicBlock *forincbb =
        llvm::BasicBlock::Create(irs->context(), "forinc", irs->topfunc());
    llvm::BasicBlock *endbb =
        llvm::BasicBlock::Create(irs->context(), "endfor", irs->topfunc());

    // init
    if (stmt->_init != nullptr) {
      stmt->_init->accept(this);
    }

    // move into the for condition block, ie. start the loop
    assert(!irs->scopereturned());
    llvm::BranchInst::Create(forbb, irs->scopebb());

    // In case of loops that have been rewritten to a composite statement
    // containing the initializers and then the actual loop, we need to
    // register the former as target scope start.
    Statement *scopeStart = stmt->getRelatedLabeled();
    while (ScopeStatement *scope = scopeStart->isScopeStatement()) {
      scopeStart = scope->statement;
    }
    irs->func()->scopes->pushLoopTarget(scopeStart, forincbb, endbb);

    // replace current scope
    irs->scope() = IRScope(forbb);

    // create the condition
    llvm::Value *cond_val;
    if (stmt->condition) {
      emitCoverageLinecountInc(stmt->condition->loc);
      DValue *cond_e = toElemDtor(stmt->condition);
      cond_val = DtoRVal(DtoCast(stmt->loc, cond_e, Type::tbool));
      delete cond_e;
    } else {
      cond_val = DtoConstBool(true);
    }

    // conditional branch
    assert(!irs->scopereturned());
    auto branchinst =
        llvm::BranchInst::Create(forbodybb, endbb, cond_val, irs->scopebb());
    {
      auto brweights = PGO.createProfileWeightsForLoop(stmt);
      PGO.addBranchWeights(branchinst, brweights);
    }

    // rewrite scope
    irs->scope() = IRScope(forbodybb);

    // do for body code
    PGO.emitCounterIncrement(stmt);
    if (stmt->_body) {
      stmt->_body->accept(this);
    }

    // Order the blocks in a logical order in IR
    forincbb->moveAfter(&irs->topfunc()->back());
    endbb->moveAfter(forincbb);

    // move into the for increment block
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(forincbb, irs->scopebb());
    }
    irs->scope() = IRScope(forincbb);

    // increment
    if (stmt->increment) {
      emitCoverageLinecountInc(stmt->increment->loc);
      DValue *inc = toElemDtor(stmt->increment);
      delete inc;
    }

    // loop
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(forbb, irs->scopebb());
    }

    irs->func()->scopes->popLoopTarget();

    // rewrite the scope
    irs->scope() = IRScope(endbb);

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(BreakStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("BreakStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // don't emit two terminators in a row
    // happens just before DMD generated default statements if the last case
    // terminates
    if (irs->scopereturned()) {
      return;
    }

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    if (stmt->ident) {
      IF_LOG Logger::println("ident = %s", stmt->ident->toChars());

      // Get the loop or break statement the label refers to
      Statement *targetStatement = stmt->target->statement;
      ScopeStatement *tmp;
      while ((tmp = targetStatement->isScopeStatement())) {
        targetStatement = tmp->statement;
      }

      irs->func()->scopes->breakToStatement(targetStatement);
    } else {
      irs->func()->scopes->breakToClosest();
    }

    // the break terminated this basicblock, start a new one
    llvm::BasicBlock *bb =
        llvm::BasicBlock::Create(irs->context(), "afterbreak", irs->topfunc());
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ContinueStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ContinueStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    if (stmt->ident) {
      IF_LOG Logger::println("ident = %s", stmt->ident->toChars());

      // get the loop statement the label refers to
      Statement *targetLoopStatement = stmt->target->statement;
      ScopeStatement *tmp;
      while ((tmp = targetLoopStatement->isScopeStatement())) {
        targetLoopStatement = tmp->statement;
      }

      irs->func()->scopes->continueWithLoop(targetLoopStatement);
    } else {
      irs->func()->scopes->continueWithClosest();
    }

    // the break terminated this basicblock, start a new one
    llvm::BasicBlock *bb =
        llvm::BasicBlock::Create(irs->context(), "afterbreak", irs->topfunc());
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(OnScopeStatement *stmt) LLVM_OVERRIDE {
    stmt->error("Internal Compiler Error: OnScopeStatement should have been "
                "lowered by frontend.");
    fatal();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(TryFinallyStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("TryFinallyStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    /*auto entryCount = */ PGO.setCurrentStmt(stmt);

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    // We only need to consider exception handling/cleanup issues if there
    // is both a try and a finally block. If not, just directly emit what
    // is present.
    if (!stmt->_body || !stmt->finalbody) {
      if (stmt->_body) {
        irs->DBuilder.EmitBlockStart(stmt->_body->loc);
        stmt->_body->accept(this);
        irs->DBuilder.EmitBlockEnd();
      } else if (stmt->finalbody) {
        irs->DBuilder.EmitBlockStart(stmt->finalbody->loc);
        stmt->finalbody->accept(this);
        irs->DBuilder.EmitBlockEnd();
      }
      return;
    }

    // We'll append the "try" part to the current basic block later. No need
    // for an extra one (we'd need to branch to it unconditionally anyway).
    llvm::BasicBlock *trybb = irs->scopebb();

    // Emit the finally block and set up the cleanup scope for it.
    llvm::BasicBlock *finallybb =
        llvm::BasicBlock::Create(irs->context(), "finally", irs->topfunc());
    irs->scope() = IRScope(finallybb);
    irs->DBuilder.EmitBlockStart(stmt->finalbody->loc);
    stmt->finalbody->accept(this);
    irs->DBuilder.EmitBlockEnd();

    CleanupCursor cleanupBefore = irs->func()->scopes->currentCleanupScope();
    irs->func()->scopes->pushCleanup(finallybb, irs->scopebb());

    // Emit the try block.
    irs->scope() = IRScope(trybb);

    assert(stmt->_body);
    irs->DBuilder.EmitBlockStart(stmt->_body->loc);
    stmt->_body->accept(this);
    irs->DBuilder.EmitBlockEnd();

    // Create a block to branch to after successfully running the try block
    // and any cleanups.
    if (!irs->scopereturned()) {
      llvm::BasicBlock *successbb = llvm::BasicBlock::Create(
          irs->context(), "try.success", irs->topfunc());
      irs->func()->scopes->runCleanups(cleanupBefore, successbb);
      irs->scope() = IRScope(successbb);
      // PGO counter tracks the continuation of the try-finally statement
      PGO.emitCounterIncrement(stmt);
    }
    irs->func()->scopes->popCleanups(cleanupBefore);
  }

//////////////////////////////////////////////////////////////////////////

#if LDC_LLVM_VER >= 308
  void emitBeginCatchMSVCEH(Catch *ctch, llvm::BasicBlock *endbb,
                            llvm::CatchSwitchInst *catchSwitchInst) {
    VarDeclaration *var = ctch->var;
    // The MSVC/x86 build uses C++ exception handling
    // This needs a series of catch pads to match the exception
    // and the catch handler must be terminated by a catch return instruction
    LLValue *exnObj = nullptr;
    LLValue *cpyObj = nullptr;
    LLValue *typeDesc = nullptr;
    LLValue *clssInfo = nullptr;
    if (var) {
      // alloca storage for the variable, it always needs a place on the stack
      // do not initialize, this will be done by the C++ exception handler
      var->_init = nullptr;

      // redirect scope to avoid the generation of debug info before the
      // catchpad
      IRScope save = irs->scope();
      irs->scope() = IRScope(gIR->topallocapoint()->getParent());
      irs->scope().builder.SetInsertPoint(gIR->topallocapoint());
      DtoDeclarationExp(var);

      // catch handler will be outlined, so always treat as a nested reference
      exnObj = getIrValue(var);

      if (var->nestedrefs.dim) {
        // if variable needed in a closure, use a stack temporary and copy it
        // when caught
        cpyObj = exnObj;
        exnObj = DtoAlloca(var->type, "exnObj");
      }
      irs->scope() = save;
      irs->DBuilder.EmitStopPoint(ctch->loc); // re-set debug loc after the
                                              // SetInsertPoint(allocaInst) call
    } else if (ctch->type) {
      // catch without var
      exnObj = DtoAlloca(ctch->type, "exnObj");
    } else {
      // catch all
      exnObj = LLConstant::getNullValue(getVoidPtrType());
    }

    if (ctch->type) {
      ClassDeclaration *cd = ctch->type->toBasetype()->isClassHandle();
      typeDesc = getTypeDescriptor(*irs, cd);
      clssInfo = getIrAggr(cd)->getClassInfoSymbol();
    } else {
      // catch all
      typeDesc = LLConstant::getNullValue(getVoidPtrType());
      clssInfo = LLConstant::getNullValue(DtoType(Type::typeinfoclass->type));
    }

    // "catchpad within %switch [TypeDescriptor, 0, &caughtObject]" must be
    // first instruction
    int flags = var ? 0 : 64; // just mimicking clang here
    LLValue *args[] = {typeDesc, DtoConstUint(flags), exnObj};
    auto catchpad = irs->ir->CreateCatchPad(
        catchSwitchInst, llvm::ArrayRef<LLValue *>(args), "");
    catchSwitchInst->addHandler(irs->scopebb());

    if (cpyObj) {
      // assign the caught exception to the location in the closure
      auto val = irs->ir->CreateLoad(exnObj);
      irs->ir->CreateStore(val, cpyObj);
      exnObj = cpyObj;
    }

    // Exceptions are never rethrown by D code (but thrown again), so
    // we can leave the catch handler right away and continue execution
    // outside the catch funclet
    llvm::BasicBlock *catchhandler = llvm::BasicBlock::Create(
        irs->context(), "catchhandler", irs->topfunc());
    llvm::CatchReturnInst::Create(catchpad, catchhandler, irs->scopebb());
    irs->scope() = IRScope(catchhandler);
    auto enterCatchFn =
        getRuntimeFunction(Loc(), irs->module, "_d_eh_enter_catch");
    irs->CreateCallOrInvoke(enterCatchFn, DtoBitCast(exnObj, getVoidPtrType()),
                            clssInfo);
  }
#endif

  void visit(TryCatchStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("TryCatchStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    auto entryCount = PGO.setCurrentStmt(stmt);

    // Emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    // We'll append the "try" part to the current basic block later. No need
    // for an extra one (we'd need to branch to it unconditionally anyway).
    llvm::BasicBlock *trybb = irs->scopebb();

    // Create a basic block to branch to after leaving the try or an
    // associated catch block successfully.
    llvm::BasicBlock *endbb = llvm::BasicBlock::Create(
        irs->context(), "try.success.or.caught", irs->topfunc());

    assert(stmt->catches);

    struct CatchBlock {
      ClassDeclaration *classdecl;
      llvm::BasicBlock *BB;
      uint64_t catchcount;
    };

    llvm::SmallVector<CatchBlock, 6> catchBlocks;
    catchBlocks.reserve(stmt->catches->dim);

#if LDC_LLVM_VER >= 308
    if (useMSVCEH()) {
      ScopeStack *scopes = irs->func()->scopes;
      auto catchSwitchBlock = llvm::BasicBlock::Create(
          irs->context(), "catch.dispatch", irs->topfunc());
      llvm::BasicBlock *unwindto =
          scopes->currentCleanupScope() > 0 || scopes->currentCatchScope() > 0
              ? scopes->getLandingPad()
              : nullptr;
      auto funclet = scopes->getFunclet();
      auto catchSwitchInst = llvm::CatchSwitchInst::Create(
          funclet ? funclet : llvm::ConstantTokenNone::get(irs->context()),
          unwindto, stmt->catches->dim, "", catchSwitchBlock);

      for (auto it = stmt->catches->begin(), end = stmt->catches->end();
           it != end; ++it) {
        llvm::BasicBlock *catchBB = llvm::BasicBlock::Create(
            irs->context(), llvm::Twine("catch.") + (*it)->type->toChars(),
            irs->topfunc(), endbb);

        irs->scope() = IRScope(catchBB);
        irs->DBuilder.EmitBlockStart((*it)->loc);
        PGO.emitCounterIncrement(*it);

        emitBeginCatchMSVCEH(*it, endbb, catchSwitchInst);

        // Emit handler, if there is one. The handler is zero, for instance,
        // when building 'catch { debug foo(); }' in non-debug mode.
        if ((*it)->handler) {
          Statement_toIR((*it)->handler, irs);
        }

        if (!irs->scopereturned()) {
          irs->ir->CreateBr(endbb);
        }

        irs->DBuilder.EmitBlockEnd();
      }

      // TODO: PGO has not yet been implemented for MSVC EH, set catchCount
      // temporarily to 0
      uint64_t catchCount = 0;

      CatchBlock cb = {nullptr, catchSwitchBlock, catchCount};
      catchBlocks.push_back(cb); // just for cleanup
      scopes->pushCatch(nullptr, catchSwitchBlock);

      // if no landing pad is created, the catch blocks are unused, but
      // the verifier complains if there are catchpads without personality
      // so we can just set it unconditionally
      if (!irs->func()->func->hasPersonalityFn()) {
        const char *personality = "__CxxFrameHandler3";
        LLFunction *personalityFn =
            getRuntimeFunction(Loc(), irs->module, personality);
        irs->func()->func->setPersonalityFn(personalityFn);
      }
    } else
#endif
    {
      for (Catches::reverse_iterator it = stmt->catches->rbegin(),
                                     end = stmt->catches->rend();
           it != end; ++it) {
        llvm::BasicBlock *catchBB = llvm::BasicBlock::Create(
            irs->context(), llvm::Twine("catch.") + (*it)->type->toChars(),
            irs->topfunc(), endbb);

        irs->scope() = IRScope(catchBB);
        irs->DBuilder.EmitBlockStart((*it)->loc);
        PGO.emitCounterIncrement(*it);

        const auto enterCatchFn =
            getRuntimeFunction(Loc(), irs->module, "_d_eh_enter_catch");
        auto ptr = DtoLoad(irs->func()->getOrCreateEhPtrSlot());
        auto throwableObj = irs->ir->CreateCall(enterCatchFn, ptr);

        // For catches that use the Throwable object, create storage for it.
        // We will set it in the code that branches from the landing pads
        // (there might be more than one) to catchBB.
        auto var = (*it)->var;
        if (var) {
          // This will alloca if we haven't already and take care of nested refs
          // if there are any.
          DtoDeclarationExp(var);

          // Copy the exception reference over from the _d_eh_enter_catch return
          // value.
          DtoStore(DtoBitCast(throwableObj, DtoType((*it)->var->type)),
                   getIrLocal(var)->value);
        }

        // Emit handler, if there is one. The handler is zero, for instance,
        // when building 'catch { debug foo(); }' in non-debug mode.
        if ((*it)->handler) {
          Statement_toIR((*it)->handler, irs);
        }

        if (!irs->scopereturned()) {
          irs->ir->CreateBr(endbb);
        }

        irs->DBuilder.EmitBlockEnd();

        // PGO information, currently unused
        auto catchCount = PGO.getRegionCount(*it);

        CatchBlock cb = {(*it)->type->toBasetype()->isClassHandle(), catchBB,
                         catchCount};
        catchBlocks.push_back(cb);
      }

      // Total number of uncaught exceptions is equal to the execution count at
      // the start of the try block minus the one after the continuation.
      // uncaughtCount keeps track of the exception type mismatch count while
      // iterating through the catchBlocks list.
      auto uncaughtCount = entryCount - PGO.getRegionCount(stmt);

      // Only after emitting all the catch bodies, register the catch scopes.
      // This is so that (re)throwing inside a catch does not match later
      // catches.
      for (const auto &cb : catchBlocks) {
        auto matchWeights =
            PGO.createProfileWeights(cb.catchcount, uncaughtCount);
        // Add this exception type's match count to the uncaughtCount, because
        // these failed to match the exception types of the remaining
        // iterations.
        uncaughtCount += cb.catchcount;

        DtoResolveClass(cb.classdecl);

        irs->func()->scopes->pushCatch(
            getIrAggr(cb.classdecl)->getClassInfoSymbol(), cb.BB, matchWeights);
      }
    }

    // Emit the try block.
    irs->scope() = IRScope(trybb);

    assert(stmt->_body);
    irs->DBuilder.EmitBlockStart(stmt->_body->loc);
    stmt->_body->accept(this);
    irs->DBuilder.EmitBlockEnd();

    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(endbb, irs->scopebb());
    }

    // Now that we have done the try block, remove the catches and continue
    // codegen in the end block the try and all the catches branch to.
    for (size_t i = 0; i < catchBlocks.size(); ++i) {
      irs->func()->scopes->popCatch();
    }

    // Move end block after all generated blocks
    endbb->moveAfter(&irs->topfunc()->back());

    irs->scope() = IRScope(endbb);
    // PGO counter tracks the continuation of the try statement
    PGO.emitCounterIncrement(stmt);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ThrowStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ThrowStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    assert(stmt->exp);
    DValue *e = toElemDtor(stmt->exp);

    llvm::Function *fn =
        getRuntimeFunction(stmt->loc, irs->module, "_d_throw_exception");
    LLValue *arg =
        DtoBitCast(DtoRVal(e), fn->getFunctionType()->getParamType(0));

    irs->CreateCallOrInvoke(fn, arg);
    irs->ir->CreateUnreachable();

    // TODO: Should not be needed.
    llvm::BasicBlock *bb =
        llvm::BasicBlock::Create(irs->context(), "afterthrow", irs->topfunc());
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(SwitchStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("SwitchStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);
    auto incoming_count = PGO.getCurrentRegionCount();

    // emit dwarf stop point
    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    llvm::BasicBlock *oldbb = irs->scopebb();

    // Codegen state variables stored in the AST must be reset (see end of
    // function)
    for (CaseStatement *cs : *stmt->cases) {
      assert(cs->bodyBB == nullptr);
      assert(cs->llvmIdx == nullptr);
    }
    if (stmt->sdefault) {
      assert(stmt->sdefault->bodyBB == nullptr);
    }

    // If one of the case expressions is non-constant, we can't use
    // 'switch' instruction (that can happen because D2 allows to
    // initialize a global variable in a static constructor).
    bool useSwitchInst = true;
    for (auto cs : *stmt->cases) {
      VarDeclaration *vd = nullptr;
      if (cs->exp->op == TOKvar) {
        vd = static_cast<VarExp *>(cs->exp)->var->isVarDeclaration();
      }
      if (vd && (!vd->_init || !vd->isConst())) {
        cs->llvmIdx = DtoRVal(toElemDtor(cs->exp));
        useSwitchInst = false;
      }
    }

    // body block.
    // FIXME: that block is never used
    llvm::BasicBlock *bodybb =
        llvm::BasicBlock::Create(irs->context(), "switchbody", irs->topfunc());

    // end (break point)
    llvm::BasicBlock *endbb =
        llvm::BasicBlock::Create(irs->context(), "switchend", irs->topfunc());
    // PGO counter tracks exit point of switch statement:
    {
      irs->scope() = IRScope(endbb);
      PGO.emitCounterIncrement(stmt);
    }

    // default
    llvm::BasicBlock *defbb = nullptr;
    if (stmt->sdefault) {
      Logger::println("has default");
      defbb =
          llvm::BasicBlock::Create(irs->context(), "default", irs->topfunc());
      stmt->sdefault->bodyBB = defbb;
    }

    // do switch body
    assert(stmt->_body);
    irs->scope() = IRScope(bodybb);
    irs->func()->scopes->pushBreakTarget(stmt, endbb);
    stmt->_body->accept(this);
    irs->func()->scopes->popBreakTarget();
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(endbb, irs->scopebb());
    }

    irs->scope() = IRScope(oldbb);
    if (useSwitchInst) {
      // string switch?
      llvm::Value *switchTable = nullptr;
      std::vector<Case> caseArray;
      if (!stmt->condition->type->isintegral()) {
        Logger::println("is string switch");
        // build array of the stringexpS
        caseArray.reserve(stmt->cases->dim);
        for (unsigned i = 0; i < stmt->cases->dim; ++i) {
          CaseStatement *cs =
              static_cast<CaseStatement *>(stmt->cases->data[i]);

          assert(cs->exp->op == TOKstring);
          caseArray.emplace_back(static_cast<StringExp *>(cs->exp), i);
        }
        // first sort it
        std::sort(caseArray.begin(), caseArray.end());
        // iterate and add indices to cases
        std::vector<llvm::Constant *> inits(caseArray.size(), nullptr);
        for (size_t i = 0, e = caseArray.size(); i < e; ++i) {
          Case &c = caseArray[i];
          CaseStatement *cs =
              static_cast<CaseStatement *>(stmt->cases->data[c.index]);
          cs->llvmIdx = DtoConstUint(i);
          inits[i] = toConstElem(c.str, irs);
        }
        // build static array for ptr or final array
        llvm::Type *elemTy = DtoType(stmt->condition->type);
        LLArrayType *arrTy = llvm::ArrayType::get(elemTy, inits.size());
        LLConstant *arrInit = LLConstantArray::get(arrTy, inits);
        auto arr = new llvm::GlobalVariable(
            irs->module, arrTy, true, llvm::GlobalValue::InternalLinkage,
            arrInit, ".string_switch_table_data");

        LLType *elemPtrTy = getPtrToType(elemTy);
        LLConstant *arrPtr = llvm::ConstantExpr::getBitCast(arr, elemPtrTy);

        // build the static table
        LLType *types[] = {DtoSize_t(), elemPtrTy};
        LLStructType *sTy = llvm::StructType::get(irs->context(), types, false);
        LLConstant *sinits[] = {DtoConstSize_t(inits.size()), arrPtr};
        switchTable = llvm::ConstantStruct::get(
            sTy, llvm::ArrayRef<LLConstant *>(sinits));
      }

      // condition var
      LLValue *condVal;
      // integral switch
      if (stmt->condition->type->isintegral()) {
        DValue *cond = toElemDtor(stmt->condition);
        condVal = DtoRVal(cond);
      }
      // string switch
      else {
        condVal = call_string_switch_runtime(switchTable, stmt->condition);
      }

      // Create switch and add the cases.
      // For PGO instrumentation, we need to add counters /before/ the case
      // statement bodies, because the counters should only count the jumps
      // directly from the switch statement.
      llvm::SwitchInst *si;
      if (!global.params.genInstrProf) {
        si = llvm::SwitchInst::Create(condVal, defbb ? defbb : endbb,
                                      stmt->cases->dim, irs->scopebb());
        for (auto cs : *stmt->cases) {
          si->addCase(isaConstantInt(cs->llvmIdx), cs->bodyBB);
        }
      } else {
        auto switchbb = irs->scopebb();
        // Add PGO instrumentation.
        // Create "default" counter
        {
          llvm::BasicBlock *defaultcntr = llvm::BasicBlock::Create(
              irs->context(), "defaultcntr", irs->topfunc());
          irs->scope() = IRScope(defaultcntr);
          PGO.emitCounterIncrement(stmt->sdefault);
          llvm::BranchInst::Create(defbb ? defbb : endbb, irs->scopebb());
          defaultcntr->moveBefore(defbb ? defbb : endbb);
          // Create switch
          si = llvm::SwitchInst::Create(condVal, defaultcntr, stmt->cases->dim,
                                        switchbb);
        }
        // Create and add case counters
        for (auto cs : *stmt->cases) {
          llvm::BasicBlock *casecntr = llvm::BasicBlock::Create(
              irs->context(), "casecntr", irs->topfunc());
          irs->scope() = IRScope(casecntr);
          PGO.emitCounterIncrement(cs);
          llvm::BranchInst::Create(cs->bodyBB, irs->scopebb());
          casecntr->moveBefore(cs->bodyBB);

          si->addCase(isaConstantInt(cs->llvmIdx), casecntr);
        }
      }

      // Put the switchend block after the last block, for a more logical IR
      // layout.
      endbb->moveAfter(&irs->topfunc()->back());

      // Apply PGO switch branch weights:
      {
        // Get case statements execution counts from profile data.
        std::vector<uint64_t> case_prof_counts;
        case_prof_counts.push_back(
            stmt->sdefault ? PGO.getRegionCount(stmt->sdefault) : 0);
        for (auto cs : *stmt->cases) {
          auto w = PGO.getRegionCount(cs);
          case_prof_counts.push_back(w);
        }

        auto brweights = PGO.createProfileWeights(case_prof_counts);
        PGO.addBranchWeights(si, brweights);
      }

    } else { // we can't use switch, so we will use a bunch of br instructions
             // instead
      DValue *cond = toElemDtor(stmt->condition);
      LLValue *condVal = DtoRVal(cond);

      llvm::BasicBlock *nextbb =
          llvm::BasicBlock::Create(irs->context(), "checkcase", irs->topfunc());
      llvm::BranchInst::Create(nextbb, irs->scopebb());

      auto defaultjumptarget = defbb ? defbb : endbb;
      // Create "default:" counter for profiling
      if (global.params.genInstrProf) {
        llvm::BasicBlock *defaultcntr = llvm::BasicBlock::Create(
            irs->context(), "defaultcntr", irs->topfunc());
        irs->scope() = IRScope(defaultcntr);
        PGO.emitCounterIncrement(stmt->sdefault);
        llvm::BranchInst::Create(defbb ? defbb : endbb, irs->scopebb());
        defaultcntr->moveBefore(defbb ? defbb : endbb);
        defaultjumptarget = defaultcntr;
      }

      irs->scope() = IRScope(nextbb);
      auto failedCompareCount = incoming_count;
      for (auto cs : *stmt->cases) {
        LLValue *cmp = irs->ir->CreateICmp(llvm::ICmpInst::ICMP_EQ, cs->llvmIdx,
                                           condVal, "checkcase");
        nextbb = llvm::BasicBlock::Create(irs->context(), "checkcase",
                                          irs->topfunc());

        // Add case counters for PGO in front of case body
        auto casejumptargetbb = cs->bodyBB;
        if (global.params.genInstrProf) {
          llvm::BasicBlock *casecntr = llvm::BasicBlock::Create(
              irs->context(), "casecntr", irs->topfunc());
          auto savedbb = irs->scope();
          irs->scope() = IRScope(casecntr);
          PGO.emitCounterIncrement(cs);
          llvm::BranchInst::Create(cs->bodyBB, irs->scopebb());
          casecntr->moveBefore(cs->bodyBB);
          irs->scope() = savedbb;

          casejumptargetbb = casecntr;
        }

        // Create the comparison branch for this case
        auto branchinst = llvm::BranchInst::Create(casejumptargetbb, nextbb,
                                                   cmp, irs->scopebb());

        // Calculate and apply PGO branch weights
        {
          auto trueCount = PGO.getRegionCount(cs);
          assert(trueCount <= failedCompareCount &&
                 "Higher branch count than switch incoming count!");
          failedCompareCount -= trueCount;
          auto brweights =
              PGO.createProfileWeights(trueCount, failedCompareCount);
          PGO.addBranchWeights(branchinst, brweights);
        }

        irs->scope() = IRScope(nextbb);
      }

      llvm::BranchInst::Create(defaultjumptarget, irs->scopebb());

      endbb->moveAfter(nextbb);
    }

    irs->scope() = IRScope(endbb);

    // Reset backend variables to original state (to allow multiple codegen
    // passes of same ast nodes)
    // TODO: move the codegen state variables out of the AST.
    for (CaseStatement *cs : *stmt->cases) {
      cs->bodyBB = nullptr;
      cs->llvmIdx = nullptr;
    }
    if (stmt->sdefault) {
      stmt->sdefault->bodyBB = nullptr;
    }
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(CaseStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("CaseStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    llvm::BasicBlock *nbb =
        llvm::BasicBlock::Create(irs->context(), "case", irs->topfunc());
    if (stmt->bodyBB && !stmt->bodyBB->getTerminator()) {
      llvm::BranchInst::Create(nbb, stmt->bodyBB);
    }
    stmt->bodyBB = nbb;

    if (stmt->llvmIdx == nullptr) {
      llvm::Constant *c = toConstElem(stmt->exp, irs);
      stmt->llvmIdx = isaConstantInt(c);
    }

    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(stmt->bodyBB, irs->scopebb());
    }

    irs->scope() = IRScope(stmt->bodyBB);

    assert(stmt->statement);
    irs->DBuilder.EmitBlockStart(stmt->statement->loc);
    emitCoverageLinecountInc(stmt->loc);
    if (stmt->gototarget) {
      PGO.emitCounterIncrement(PGO.getCounterPtr(stmt, 1));
    }
    stmt->statement->accept(this);
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(DefaultStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("DefaultStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    assert(stmt->bodyBB);

    llvm::BasicBlock *nbb =
        llvm::BasicBlock::Create(irs->context(), "default", irs->topfunc());

    if (!stmt->bodyBB->getTerminator()) {
      llvm::BranchInst::Create(nbb, stmt->bodyBB);
    }
    stmt->bodyBB = nbb;

    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(stmt->bodyBB, irs->scopebb());
    }

    irs->scope() = IRScope(stmt->bodyBB);

    assert(stmt->statement);
    irs->DBuilder.EmitBlockStart(stmt->statement->loc);
    emitCoverageLinecountInc(stmt->loc);
    if (stmt->gototarget) {
      PGO.emitCounterIncrement(PGO.getCounterPtr(stmt, 1));
    }
    stmt->statement->accept(this);
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(UnrolledLoopStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("UnrolledLoopStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // if no statements, there's nothing to do
    if (!stmt->statements || !stmt->statements->dim) {
      return;
    }

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // DMD doesn't fold stuff like continue/break, and since this isn't really a
    // loop
    // we have to keep track of each statement and jump to the next/end on
    // continue/break

    // create a block for each statement
    size_t nstmt = stmt->statements->dim;
    llvm::SmallVector<llvm::BasicBlock *, 4> blocks(nstmt, nullptr);

    for (size_t i = 0; i < nstmt; i++) {
      blocks[i] = llvm::BasicBlock::Create(irs->context(), "unrolledstmt",
                                           irs->topfunc());
    }

    // create end block
    llvm::BasicBlock *endbb =
        llvm::BasicBlock::Create(irs->context(), "unrolledend", irs->topfunc());

    // enter first stmt
    if (!irs->scopereturned()) {
      irs->ir->CreateBr(blocks[0]);
    }

    // do statements
    Statement **stmts = static_cast<Statement **>(stmt->statements->data);

    for (size_t i = 0; i < nstmt; i++) {
      Statement *s = stmts[i];

      // get blocks
      llvm::BasicBlock *thisbb = blocks[i];
      llvm::BasicBlock *nextbb = (i + 1 == nstmt) ? endbb : blocks[i + 1];

      // update scope
      irs->scope() = IRScope(thisbb);

      // push loop scope
      // continue goes to next statement, break goes to end
      irs->func()->scopes->pushLoopTarget(stmt, nextbb, endbb);

      // do statement
      s->accept(this);

      // pop loop scope
      irs->func()->scopes->popLoopTarget();

      // next stmt
      if (!irs->scopereturned()) {
        irs->ir->CreateBr(nextbb);
      }
    }

    // finish scope
    if (!irs->scopereturned()) {
      irs->ir->CreateBr(endbb);
    }
    irs->scope() = IRScope(endbb);

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ForeachStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ForeachStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // assert(arguments->dim == 1);
    assert(stmt->value != 0);
    assert(stmt->aggr != 0);
    assert(stmt->func != 0);

    // Argument* arg = static_cast<Argument*>(arguments->data[0]);
    // Logger::println("Argument is %s", arg->toChars());

    IF_LOG Logger::println("aggr = %s", stmt->aggr->toChars());

    // key
    LLType *keytype = stmt->key ? DtoType(stmt->key->type) : DtoSize_t();
    LLValue *keyvar;
    if (stmt->key) {
      keyvar = DtoRawVarDeclaration(stmt->key);
    } else {
      keyvar = DtoRawAlloca(keytype, 0, "foreachkey");
    }
    LLValue *zerokey = LLConstantInt::get(keytype, 0, false);

    // value
    IF_LOG Logger::println("value = %s", stmt->value->toPrettyChars());
    LLValue *valvar = nullptr;
    if (!stmt->value->isRef() && !stmt->value->isOut()) {
      // Create a local variable to serve as the value.
      DtoRawVarDeclaration(stmt->value);
      valvar = getIrLocal(stmt->value)->value;
    }

    // what to iterate
    DValue *aggrval = toElemDtor(stmt->aggr);

    // get length and pointer
    LLValue *niters = DtoArrayLen(aggrval);
    LLValue *val = DtoArrayPtr(aggrval);

    if (niters->getType() != keytype) {
      size_t sz1 = getTypeBitSize(niters->getType());
      size_t sz2 = getTypeBitSize(keytype);
      if (sz1 < sz2) {
        niters = irs->ir->CreateZExt(niters, keytype, "foreachtrunckey");
      } else if (sz1 > sz2) {
        niters = irs->ir->CreateTrunc(niters, keytype, "foreachtrunckey");
      } else {
        niters = irs->ir->CreateBitCast(niters, keytype, "foreachtrunckey");
      }
    }

    if (stmt->op == TOKforeach) {
      new llvm::StoreInst(zerokey, keyvar, irs->scopebb());
    } else {
      new llvm::StoreInst(niters, keyvar, irs->scopebb());
    }

    llvm::BasicBlock *condbb =
        llvm::BasicBlock::Create(irs->context(), "foreachcond", irs->topfunc());
    llvm::BasicBlock *bodybb =
        llvm::BasicBlock::Create(irs->context(), "foreachbody", irs->topfunc());
    llvm::BasicBlock *nextbb =
        llvm::BasicBlock::Create(irs->context(), "foreachnext", irs->topfunc());
    llvm::BasicBlock *endbb =
        llvm::BasicBlock::Create(irs->context(), "foreachend", irs->topfunc());

    llvm::BranchInst::Create(condbb, irs->scopebb());

    // condition
    irs->scope() = IRScope(condbb);

    LLValue *done = nullptr;
    LLValue *load = DtoLoad(keyvar);
    if (stmt->op == TOKforeach) {
      done = irs->ir->CreateICmpULT(load, niters);
    } else if (stmt->op == TOKforeach_reverse) {
      done = irs->ir->CreateICmpUGT(load, zerokey);
      load = irs->ir->CreateSub(load, LLConstantInt::get(keytype, 1, false));
      DtoStore(load, keyvar);
    }
    auto branchinst =
        llvm::BranchInst::Create(bodybb, endbb, done, irs->scopebb());
    {
      auto brweights = PGO.createProfileWeightsForeach(stmt);
      PGO.addBranchWeights(branchinst, brweights);
    }

    // init body
    irs->scope() = IRScope(bodybb);
    PGO.emitCounterIncrement(stmt);

    // get value for this iteration
    LLValue *loadedKey = irs->ir->CreateLoad(keyvar);
    LLValue *gep = DtoGEP1(val, loadedKey, true);

    if (!stmt->value->isRef() && !stmt->value->isOut()) {
      // Copy value to local variable, and use it as the value variable.
      DLValue dst(stmt->value->type, valvar);
      DLValue src(stmt->value->type, gep);
      DtoAssign(stmt->loc, &dst, &src, TOKassign);
      getIrLocal(stmt->value)->value = valvar;
    } else {
      // Use the GEP as the address of the value variable.
      DtoRawVarDeclaration(stmt->value, gep);
    }

    // emit body
    irs->func()->scopes->pushLoopTarget(stmt, nextbb, endbb);
    if (stmt->_body) {
      stmt->_body->accept(this);
    }
    irs->func()->scopes->popLoopTarget();

    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(nextbb, irs->scopebb());
    }

    // next
    irs->scope() = IRScope(nextbb);
    if (stmt->op == TOKforeach) {
      LLValue *load = DtoLoad(keyvar);
      load = irs->ir->CreateAdd(load, LLConstantInt::get(keytype, 1, false));
      DtoStore(load, keyvar);
    }
    llvm::BranchInst::Create(condbb, irs->scopebb());

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();

    // end
    irs->scope() = IRScope(endbb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ForeachRangeStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("ForeachRangeStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // start a dwarf lexical block
    irs->DBuilder.EmitBlockStart(stmt->loc);

    // evaluate lwr/upr
    assert(stmt->lwr->type->isintegral());
    LLValue *lower = DtoRVal(toElemDtor(stmt->lwr));
    assert(stmt->upr->type->isintegral());
    LLValue *upper = DtoRVal(toElemDtor(stmt->upr));

    // handle key
    assert(stmt->key->type->isintegral());
    LLValue *keyval = DtoRawVarDeclaration(stmt->key);

    // store initial value in key
    if (stmt->op == TOKforeach) {
      DtoStore(lower, keyval);
    } else {
      DtoStore(upper, keyval);
    }

    // set up the block we'll need
    llvm::BasicBlock *condbb = llvm::BasicBlock::Create(
        irs->context(), "foreachrange_cond", irs->topfunc());
    llvm::BasicBlock *bodybb = llvm::BasicBlock::Create(
        irs->context(), "foreachrange_body", irs->topfunc());
    llvm::BasicBlock *nextbb = llvm::BasicBlock::Create(
        irs->context(), "foreachrange_next", irs->topfunc());
    llvm::BasicBlock *endbb = llvm::BasicBlock::Create(
        irs->context(), "foreachrange_end", irs->topfunc());

    // jump to condition
    llvm::BranchInst::Create(condbb, irs->scopebb());

    // CONDITION
    irs->scope() = IRScope(condbb);

    // first we test that lwr < upr
    lower = DtoLoad(keyval);
    assert(lower->getType() == upper->getType());
    llvm::ICmpInst::Predicate cmpop;
    if (isLLVMUnsigned(stmt->key->type)) {
      cmpop = (stmt->op == TOKforeach) ? llvm::ICmpInst::ICMP_ULT
                                       : llvm::ICmpInst::ICMP_UGT;
    } else {
      cmpop = (stmt->op == TOKforeach) ? llvm::ICmpInst::ICMP_SLT
                                       : llvm::ICmpInst::ICMP_SGT;
    }
    LLValue *cond = irs->ir->CreateICmp(cmpop, lower, upper);

    // jump to the body if range is ok, to the end if not
    auto branchinst =
        llvm::BranchInst::Create(bodybb, endbb, cond, irs->scopebb());
    {
      auto brweights = PGO.createProfileWeightsForeachRange(stmt);
      PGO.addBranchWeights(branchinst, brweights);
    }

    // BODY
    irs->scope() = IRScope(bodybb);
    PGO.emitCounterIncrement(stmt);

    // reverse foreach decrements here
    if (stmt->op == TOKforeach_reverse) {
      LLValue *v = DtoLoad(keyval);
      LLValue *one = LLConstantInt::get(v->getType(), 1, false);
      v = irs->ir->CreateSub(v, one);
      DtoStore(v, keyval);
    }

    // emit body
    irs->func()->scopes->pushLoopTarget(stmt, nextbb, endbb);
    if (stmt->_body) {
      stmt->_body->accept(this);
    }
    irs->func()->scopes->popLoopTarget();

    // jump to next iteration
    if (!irs->scopereturned()) {
      llvm::BranchInst::Create(nextbb, irs->scopebb());
    }

    // NEXT
    irs->scope() = IRScope(nextbb);

    // forward foreach increments here
    if (stmt->op == TOKforeach) {
      LLValue *v = DtoLoad(keyval);
      LLValue *one = LLConstantInt::get(v->getType(), 1, false);
      v = irs->ir->CreateAdd(v, one);
      DtoStore(v, keyval);
    }

    // jump to condition
    llvm::BranchInst::Create(condbb, irs->scopebb());

    // end the dwarf lexical block
    irs->DBuilder.EmitBlockEnd();

    // END
    irs->scope() = IRScope(endbb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(LabelStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("LabelStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    // if it's an inline asm label, we don't create a basicblock, just emit it
    // in the asm
    if (irs->asmBlock) {
      auto a = new IRAsmStmt;
      std::stringstream label;
      printLabelName(label, mangleExact(irs->func()->decl),
                     stmt->ident->toChars());
      label << ":";
      a->code = label.str();
      irs->asmBlock->s.push_back(a);
      irs->asmBlock->internalLabels.push_back(stmt->ident);

      // disable inlining
      irs->func()->setNeverInline();
    } else {
      llvm::BasicBlock *labelBB = llvm::BasicBlock::Create(
          irs->context(), llvm::Twine("label.") + stmt->ident->toChars(),
          irs->topfunc());
      irs->func()->scopes->addLabelTarget(stmt->ident, labelBB);

      if (!irs->scopereturned()) {
        llvm::BranchInst::Create(labelBB, irs->scopebb());
      }

      irs->scope() = IRScope(labelBB);
    }

    PGO.emitCounterIncrement(stmt);
    // statement == nullptr when the label is at the end of function
    if (stmt->statement) {
      stmt->statement->accept(this);
    }
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(GotoStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("GotoStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    DtoGoto(stmt->loc, stmt->label);

    // TODO: Should not be needed.
    llvm::BasicBlock *bb =
        llvm::BasicBlock::Create(irs->context(), "aftergoto", irs->topfunc());
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(GotoDefaultStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("GotoDefaultStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    assert(!irs->scopereturned());
    assert(stmt->sw->sdefault->bodyBB);

#if 0
        // TODO: Store switch scopes.
        DtoEnclosingHandlers(stmt->loc, stmt->sw);
#endif

    llvm::BranchInst::Create(stmt->sw->sdefault->bodyBB, irs->scopebb());

    // TODO: Should not be needed.
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(
        irs->context(), "aftergotodefault", irs->topfunc());
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(GotoCaseStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("GotoCaseStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    irs->DBuilder.EmitStopPoint(stmt->loc);

    emitCoverageLinecountInc(stmt->loc);

    assert(!irs->scopereturned());
    if (!stmt->cs->bodyBB) {
      stmt->cs->bodyBB =
          llvm::BasicBlock::Create(irs->context(), "goto_case", irs->topfunc());
    }

#if 0
        // TODO: Store switch scopes.
        DtoEnclosingHandlers(stmt->loc, stmt->sw);
#endif

    llvm::BranchInst::Create(stmt->cs->bodyBB, irs->scopebb());

    // TODO: Should not be needed.
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(
        irs->context(), "aftergotocase", irs->topfunc());
    irs->scope() = IRScope(bb);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(WithStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("WithStatement::toIR(): %s", stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    irs->DBuilder.EmitBlockStart(stmt->loc);

    assert(stmt->exp);

    // with(..) can either be used with expressions or with symbols
    // wthis == null indicates the symbol form
    if (stmt->wthis) {
      LLValue *mem = DtoRawVarDeclaration(stmt->wthis);
      DValue *e = toElemDtor(stmt->exp);
      LLValue *val = (DtoIsInMemoryOnly(e->type) ? DtoLVal(e) : DtoRVal(e));
      DtoStore(val, mem);
    }

    if (stmt->_body) {
      stmt->_body->accept(this);
    }

    irs->DBuilder.EmitBlockEnd();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(SwitchErrorStatement *stmt) LLVM_OVERRIDE {
    IF_LOG Logger::println("SwitchErrorStatement::toIR(): %s",
                           stmt->loc.toChars());
    LOG_SCOPE;

    auto &PGO = irs->func()->pgo;
    PGO.setCurrentStmt(stmt);

    llvm::Function *fn =
        getRuntimeFunction(stmt->loc, irs->module, "_d_switch_error");

    LLValue *moduleInfoSymbol =
        getIrModule(irs->func()->decl->getModule())->moduleInfoSymbol();
    LLType *moduleInfoType = DtoType(Module::moduleinfo->type);

    LLCallSite call = irs->CreateCallOrInvoke(
        fn, DtoBitCast(moduleInfoSymbol, getPtrToType(moduleInfoType)),
        DtoConstUint(stmt->loc.linnum));
    call.setDoesNotReturn();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(AsmStatement *stmt) LLVM_OVERRIDE { AsmStatement_toIR(stmt, irs); }

  //////////////////////////////////////////////////////////////////////////

  void visit(CompoundAsmStatement *stmt) LLVM_OVERRIDE {
    CompoundAsmStatement_toIR(stmt, irs);
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(ImportStatement *stmt) LLVM_OVERRIDE {
    // Empty.
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(Statement *stmt) LLVM_OVERRIDE {
    error(stmt->loc, "Statement type Statement not implemented: %s",
          stmt->toChars());
    fatal();
  }

  //////////////////////////////////////////////////////////////////////////

  void visit(PragmaStatement *stmt) LLVM_OVERRIDE {
    error(stmt->loc, "Statement type PragmaStatement not implemented: %s",
          stmt->toChars());
    fatal();
  }
};

//////////////////////////////////////////////////////////////////////////////

void Statement_toIR(Statement *s, IRState *irs) {
  ToIRVisitor v(irs);
  s->accept(&v);
}
