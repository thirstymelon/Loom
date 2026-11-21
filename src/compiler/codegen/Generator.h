#pragma once
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "../../targets/TargetProfile.h"

#include "../eval/ConstEvaluator.h"

namespace loom {
class Generator {
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module;
    llvm::IRBuilder<> builder{context};
    llvm::Function* current = nullptr;
    const Function* currentFn = nullptr;
    llvm::Function* writeCharacterFunction = nullptr;
    bool reachable = false;
    std::unordered_map<std::string, llvm::Value*> storage;
    std::unordered_map<std::string, Type> types;
    std::unordered_map<std::string, llvm::Function*> functions;
    std::unordered_map<std::string, llvm::GlobalVariable*> globalStorage;
    std::unordered_map<std::string, llvm::StructType*> recordStorageTypes;
    unsigned stringCounter = 0;
    std::string sourcePath;
    std::unordered_map<std::string, llvm::Constant*> stringPool;
    llvm::Constant* getStringLiteral(const std::string& str) {
        auto it = stringPool.find(str);
        if (it != stringPool.end()) return it->second;
        auto* data = llvm::ConstantDataArray::getString(context, str, true);
        auto* global = new llvm::GlobalVariable(*module, data->getType(), true,
            llvm::GlobalValue::PrivateLinkage, data, ".loom.str." + std::to_string(++stringCounter));
        llvm::Constant* indices[] = {llvm::ConstantInt::get(builder.getInt32Ty(), 0),
                                     llvm::ConstantInt::get(builder.getInt32Ty(), 0)};
        auto* ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(data->getType(), global, indices);
        stringPool[str] = ptr;
        return ptr;
    }
    struct LoopTarget {
        llvm::BasicBlock* exit;
        llvm::BasicBlock* next;
        bool hasExit = false;
        std::size_t ensureCount = 0;
    };
    std::vector<LoopTarget> loops;
    std::vector<const Stmt*> activeEnsures;
    std::unordered_map<std::string, const RecordDecl*> recordDecls;
    std::unordered_map<std::string, const Function*> functionDecls;
    std::vector<std::pair<std::string, Type>> currentFunctionDrops;

    void emitScopeDrops(const std::string& skipVar = "") {
        for (auto it = currentFunctionDrops.rbegin(); it != currentFunctionDrops.rend(); ++it) {
            if (!skipVar.empty() && it->first == skipVar) continue;
            auto slotIt = storage.find(it->first);
            if (slotIt == storage.end()) continue;
            llvm::Value* slot = slotIt->second;
            if (it->second.kind == TypeKind::Vector) {
                auto freeFn = module->getOrInsertFunction("loom_vec_free", llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false));
                builder.CreateCall(freeFn, {slot});
            } else if (it->second.kind == TypeKind::Text) {
                auto freeFn = module->getOrInsertFunction("loom_text_free", llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false));
                builder.CreateCall(freeFn, {slot});
            } else if (it->second.kind == TypeKind::HMap) {
                llvm::Value* mapPtr = builder.CreateLoad(builder.getPtrTy(), slot);
                auto freeFn = module->getOrInsertFunction("loom_hmap_free", llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false));
                builder.CreateCall(freeFn, {mapPtr});
            } else if (it->second.kind == TypeKind::FileHandle) {
                llvm::Value* fhPtr = builder.CreateLoad(builder.getPtrTy(), slot);
                auto freeFn = module->getOrInsertFunction("loom_file_close", llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy()}, false));
                builder.CreateCall(freeFn, {fhPtr});
            } else if (it->second.kind == TypeKind::Record) {
                std::string dropFnName = "__loom_drop_" + it->second.name;
                auto* dropFn = module->getFunction(dropFnName);
                if (dropFn) {
                    builder.CreateCall(dropFn, {slot});
                }
            } else if (it->second.kind == TypeKind::SmartPointer) {
                llvm::Value* smtPtr = builder.CreateLoad(builder.getPtrTy(), slot);
                auto relFn = module->getOrInsertFunction("loom_smt_release", llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false));
                builder.CreateCall(relFn, {smtPtr});
            }
        }
    }
    const Analyzer& analyzer;
    bool releaseMode = false;
    std::shared_ptr<TargetProfile> targetProfile;
    bool baremetal = false;
    OptReport* optReport = nullptr;
    bool emitDebug = false;
    std::unique_ptr<llvm::DIBuilder> dibuilder;
    llvm::DICompileUnit* diCompileUnit = nullptr;
    llvm::DIFile* diFile = nullptr;
    std::vector<llvm::DIScope*> diScopes;

    void setDebugLoc(int line, int col = 1) {
        if (emitDebug && dibuilder && !diScopes.empty() && line > 0) {
            builder.SetCurrentDebugLocation(llvm::DILocation::get(context, line, col > 0 ? col : 1, diScopes.back()));
        }
    }
    std::unique_ptr<ConstEvaluator> constEvaluator;

    bool isParallelSafeLoop(const Stmt& s, std::string& inductionVar) const {
        if (s.kind != Stmt::Kind::Loop || !s.whileLoop || !s.expr) return false;
        if (s.expr->kind != Expr::Kind::Binary) return false;
        if (s.expr->text != "<" && s.expr->text != "<=") return false;
        if (!s.expr->left || s.expr->left->kind != Expr::Kind::Variable) return false;
        inductionVar = s.expr->left->text;
        if (s.body.size() < 2) return false;
        const auto& lastStmt = s.body.back();
        bool stepValid = false;
        if (lastStmt.kind == Stmt::Kind::Assign && lastStmt.name == inductionVar) {
            if (lastStmt.expr && lastStmt.expr->kind == Expr::Kind::Binary && lastStmt.expr->text == "+") {
                if (lastStmt.expr->left && lastStmt.expr->left->kind == Expr::Kind::Variable &&
                    lastStmt.expr->left->text == inductionVar) {
                    stepValid = true;
                }
            }
        }
        if (!stepValid) return false;
        for (std::size_t i = 0; i + 1 < s.body.size(); ++i) {
            const auto& st = s.body[i];
            if (st.kind == Stmt::Kind::Exit || st.kind == Stmt::Kind::Skip ||
                st.kind == Stmt::Kind::Give || st.kind == Stmt::Kind::Ensure ||
                st.kind == Stmt::Kind::Loop || st.kind == Stmt::Kind::DerefAssign ||
                st.kind == Stmt::Kind::Write) return false;
            if (st.kind == Stmt::Kind::Assign) {
                if (st.name == inductionVar) return false;
                if (st.target) {
                    if (st.target->kind == Expr::Kind::Index) {
                        if (!st.target->right || st.target->right->kind != Expr::Kind::Variable ||
                            st.target->right->text != inductionVar) {
                            return false;
                        }
                    } else {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    llvm::Type* loomType(Type t) { return llvmType(context, t, &recordStorageTypes); }
    bool terminated() const {
        auto* b = builder.GetInsertBlock();
        return !b || (!b->empty() && b->back().isTerminator());
    }
    llvm::Value* load(const std::string& name) {
        auto i = storage.find(name);
        if (i != storage.end()) {
            bool isVol = types.at(name).isVolatile;
            return builder.CreateLoad(loomType(types.at(name)), i->second, isVol, name);
        }
        auto g = globalStorage.find(name);
        if (g != globalStorage.end()) {
            bool isVol = analyzer.typeOfGlobal(name).isVolatile;
            return builder.CreateLoad(g->second->getValueType(), g->second, isVol, name);
        }
        throw Error("internal error: missing variable '" + name + "'");
    }
    llvm::Value* emitCast(llvm::Value* val, Type src, Type dst) {
        if (src == dst) return val;
        llvm::Type* dstLlvm = loomType(dst);
        if ((src.kind == TypeKind::Pointer || src.kind == TypeKind::NullType) &&
            (dst.kind == TypeKind::Pointer || dst.kind == TypeKind::NullType))
            return val;
        if (src.kind == TypeKind::Pointer && dst.integer())
            return builder.CreatePtrToInt(val, dstLlvm, "ptrtoint");
        if (src.integer() && dst.kind == TypeKind::Pointer)
            return builder.CreateIntToPtr(val, dstLlvm, "inttoptr");
        if (src.kind == TypeKind::Float && dst.kind == TypeKind::Float) {
            if (src.bits < dst.bits) return builder.CreateFPExt(val, dstLlvm, "fpext");
            if (src.bits > dst.bits) return builder.CreateFPTrunc(val, dstLlvm, "fptrunc");
            return val;
        }
        if (src.kind == TypeKind::Float && (dst.integer() || dst.kind == TypeKind::Enum || dst.kind == TypeKind::Character)) {
            if (dst.kind == TypeKind::Signed || (dst.kind == TypeKind::Enum && dst.enumSigned))
                return builder.CreateFPToSI(val, dstLlvm, "fptosi");
            else
                return builder.CreateFPToUI(val, dstLlvm, "fptoui");
        }
        if ((src.integer() || src.kind == TypeKind::Enum || src.kind == TypeKind::Character) && dst.kind == TypeKind::Float) {
            if (src.kind == TypeKind::Signed || (src.kind == TypeKind::Enum && src.enumSigned))
                return builder.CreateSIToFP(val, dstLlvm, "sitofp");
            else
                return builder.CreateUIToFP(val, dstLlvm, "uitofp");
        }
        if (src.kind == TypeKind::Boolean && (dst.integer() || dst.kind == TypeKind::Enum))
            return builder.CreateZExt(val, dstLlvm, "booltoint");
        if ((src.integer() || src.kind == TypeKind::Enum) && dst.kind == TypeKind::Boolean)
            return builder.CreateICmpNE(val, llvm::ConstantInt::get(val->getType(), 0), "inttobool");
        if ((src.integer() || src.kind == TypeKind::Enum || src.kind == TypeKind::Character) &&
            (dst.integer() || dst.kind == TypeKind::Enum || dst.kind == TypeKind::Character)) {
            unsigned srcBits = src.kind == TypeKind::Character ? 32 : src.bits;
            unsigned dstBits = dst.kind == TypeKind::Character ? 32 : dst.bits;
            if (srcBits < dstBits) {
                if (src.kind == TypeKind::Signed || (src.kind == TypeKind::Enum && src.enumSigned))
                    return builder.CreateSExt(val, dstLlvm, "sext");
                else
                    return builder.CreateZExt(val, dstLlvm, "zext");
            }
            if (srcBits > dstBits)
                return builder.CreateTrunc(val, dstLlvm, "trunc");
            return val;
        }
        throw Error("internal error: unsupported cast code generation");
    }
    llvm::Value* address(const Expr& e) {
        if (e.kind == Expr::Kind::Call) {
            auto* temp = builder.CreateAlloca(loomType(analyzer.typeOf(e)), nullptr, "call.res.addr");
            builder.CreateStore(emit(e), temp);
            return temp;
        }
        if (e.kind == Expr::Kind::ArrayLiteral) {
            auto* temp = builder.CreateAlloca(loomType(analyzer.typeOf(e)), nullptr, "arr.lit.addr");
            builder.CreateStore(emit(e), temp);
            return temp;
        }
        if (e.kind == Expr::Kind::RecordLiteral) {
            auto* temp = builder.CreateAlloca(loomType(analyzer.typeOf(e)), nullptr, "rec.lit.addr");
            builder.CreateStore(emit(e), temp);
            return temp;
        }
        if (e.kind == Expr::Kind::Borrow || e.kind == Expr::Kind::Move) {
            return address(*e.left);
        }
        if (e.kind == Expr::Kind::Deref) {
            return emit(*e.left);
        }
        if (e.kind == Expr::Kind::Variable) {
            auto local = storage.find(e.text);
            if (local != storage.end()) return local->second;
            auto global = globalStorage.find(e.text);
            if (global != globalStorage.end()) return global->second;
        }
        if (e.kind == Expr::Kind::Member) {
            Type record = analyzer.typeOf(*e.left);
            return builder.CreateStructGEP(loomType(record), address(*e.left), analyzer.fieldIndex(record.name, e.text), e.text + ".addr");
        }
        if (e.kind == Expr::Kind::Index) {
            Type targetType = analyzer.typeOf(*e.left);
            llvm::Value* idx = emit(*e.right);
            if (idx->getType() != builder.getInt64Ty())
                idx = builder.CreateZExtOrTrunc(idx, builder.getInt64Ty());
            llvm::Value* idx0 = builder.CreateSub(idx, builder.getInt64(1), "idx0");

            bool staticallyInBounds = false;
            if (e.right->kind == Expr::Kind::Integer) {
                if (targetType.kind == TypeKind::Array && e.right->magnitude >= 1 && e.right->magnitude <= targetType.arraySize) {
                    staticallyInBounds = true;
                    if (optReport && optReport->enabled) {
                        optReport->boundsEliminations.push_back(
                            "Static constant index " + std::to_string(e.right->magnitude) +
                            " in [1, " + std::to_string(targetType.arraySize) + "]: bounds check eliminated"
                        );
                    }
                }
            }

            if (!staticallyInBounds) {
                llvm::Value* lenVal = nullptr;
                if (targetType.kind == TypeKind::Array) {
                    lenVal = builder.getInt64(targetType.arraySize);
                } else if (targetType.kind == TypeKind::Slice) {
                    llvm::Value* sliceVal = emit(*e.left);
                    lenVal = builder.CreateExtractValue(sliceVal, 1, "slice.len");
                } else if (targetType.kind == TypeKind::String) {
                    llvm::Value* stringVal = emit(*e.left);
                    auto* strlenType = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
                    auto strlenCallee = module->getOrInsertFunction("strlen", strlenType);
                    lenVal = builder.CreateCall(strlenCallee, {stringVal}, "string.len");
                }
                if (lenVal) {
                    llvm::Value* ge1 = builder.CreateICmpSGE(idx, builder.getInt64(1), "check.ge1");
                    llvm::Value* leLen = builder.CreateICmpSLE(idx, lenVal, "check.lelen");
                    llvm::Value* inBounds = builder.CreateAnd(ge1, leLen, "in.bounds");
                    auto* trapBB = llvm::BasicBlock::Create(context, "bounds.trap", current);
                    auto* contBB = llvm::BasicBlock::Create(context, "bounds.ok", current);
                    builder.CreateCondBr(inBounds, contBB, trapBB);
                    builder.SetInsertPoint(trapBB);
                    auto* boundsFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getInt64Ty(), builder.getInt64Ty()}, false);
                    auto boundsCallee = module->getOrInsertFunction("loom_panic_bounds", boundsFnTy);
                    builder.CreateCall(boundsCallee, {getStringLiteral(sourcePath), builder.getInt32(e.line), idx, lenVal});
                    builder.CreateUnreachable();
                    builder.SetInsertPoint(contBB);
                }
            }

            if (targetType.kind == TypeKind::Vector) {
                llvm::Value* vecAddr = address(*e.left);
                auto* getFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                auto getCallee = module->getOrInsertFunction("loom_vec_get", getFnTy);
                return builder.CreateCall(getCallee, {vecAddr, idx}, "vec.elem.addr");
            }
            if (targetType.kind == TypeKind::Array) {
                llvm::Value* baseAddr = address(*e.left);
                llvm::Value* zero = builder.getInt64(0);
                return builder.CreateInBoundsGEP(loomType(targetType), baseAddr, {zero, idx0}, "array.elem.addr");
            }
            if (targetType.kind == TypeKind::Slice) {
                llvm::Value* sliceVal = emit(*e.left);
                llvm::Value* dataPtr = builder.CreateExtractValue(sliceVal, 0, "slice.ptr");
                Type elemType = *targetType.pointeeType;
                return builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {idx0}, "slice.elem.addr");
            }
            if (targetType.kind == TypeKind::Pointer) {
                llvm::Value* dataPtr = emit(*e.left);
                Type elemType = *targetType.pointeeType;
                return builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {idx0}, "ptr.elem.addr");
            }
            if (targetType.kind == TypeKind::String) {
                llvm::Value* strPtr = emit(*e.left);
                return builder.CreateInBoundsGEP(builder.getInt8Ty(), strPtr, {idx0}, "string.elem.addr");
            }
        }
        throw Error("internal error: expression is not an addressable lvalue");
    }

    std::unordered_map<std::string, llvm::Function*> taskTrampolines;

    llvm::FunctionCallee getLoomAlloc() {
        auto* fnType = llvm::FunctionType::get(
            builder.getPtrTy(),
            {builder.getInt64Ty(), builder.getInt32Ty(), builder.getInt32Ty()},
            false
        );
        return module->getOrInsertFunction("loom_alloc", fnType);
    }
    llvm::FunctionCallee getLoomRealloc() {
        auto* fnType = llvm::FunctionType::get(
            builder.getPtrTy(),
            {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt32Ty(), builder.getInt32Ty()},
            false
        );
        return module->getOrInsertFunction("loom_realloc", fnType);
    }
    llvm::FunctionCallee getLoomFree() {
        auto* fnType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {builder.getPtrTy()},
            false
        );
        return module->getOrInsertFunction("loom_free", fnType);
    }
    llvm::FunctionCallee getLoomTaskSpawn() {
        auto* fnType = llvm::FunctionType::get(
            builder.getPtrTy(),
            {builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty(), builder.getInt64Ty()},
            false
        );
        return module->getOrInsertFunction("loom_task_spawn", fnType);
    }
    llvm::FunctionCallee getLoomTaskJoin() {
        auto* fnType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {builder.getPtrTy(), builder.getPtrTy()},
            false
        );
        return module->getOrInsertFunction("loom_task_join", fnType);
    }

    llvm::Function* getOrCreateTaskTrampoline(const std::string& targetName, const Signature& sig) {
        std::string trampName = "__loom_task_trampoline_" + targetName;
        if (auto* existing = module->getFunction(trampName)) return existing;

        auto* trampType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {builder.getPtrTy(), builder.getPtrTy()},
            false
        );
        auto* trampFn = llvm::Function::Create(trampType, llvm::Function::InternalLinkage, trampName, *module);
        auto* prevBB = builder.GetInsertBlock();
        auto* bb = llvm::BasicBlock::Create(context, "entry", trampFn);
        llvm::IRBuilder<> b(bb);

        auto* argsPtr = trampFn->getArg(0);
        auto* resPtr = trampFn->getArg(1);

        auto fit = functions.find(targetName);
        if (fit == functions.end()) {
            throw Error("internal error: missing target function for task trampoline '" + targetName + "'");
        }
        llvm::Function* targetFunc = fit->second;

        std::vector<llvm::Value*> callArgs;
        if (!sig.parameters.empty()) {
            std::vector<llvm::Type*> fieldTypes;
            for (const auto& pt : sig.parameters) {
                fieldTypes.push_back(loomType(pt));
            }
            auto* argsStructType = llvm::StructType::get(context, fieldTypes);
            for (unsigned i = 0; i < sig.parameters.size(); ++i) {
                auto* fieldPtr = b.CreateStructGEP(argsStructType, argsPtr, i, "arg." + std::to_string(i));
                auto* argVal = b.CreateLoad(loomType(sig.parameters[i]), fieldPtr, "loaded.arg." + std::to_string(i));
                callArgs.push_back(argVal);
            }
        }

        llvm::Value* retVal = b.CreateCall(targetFunc, callArgs, targetFunc->getReturnType()->isVoidTy() ? "" : "task.call");
        if (!targetFunc->getReturnType()->isVoidTy() && sig.result && sig.result->kind != TypeKind::NullType) {
            b.CreateStore(retVal, resPtr);
        }
        b.CreateRetVoid();

        if (prevBB) builder.SetInsertPoint(prevBB);
        taskTrampolines[targetName] = trampFn;
        return trampFn;
    }

    llvm::Value* emit(const Expr& e) {
        if (e.kind == Expr::Kind::Move) {
            llvm::Value* val = emit(*e.left);
            if (e.left && e.left->kind == Expr::Kind::Variable) {
                if (storage.count(e.left->text)) {
                    builder.CreateStore(llvm::Constant::getNullValue(loomType(analyzer.typeOf(*e.left))), storage.at(e.left->text));
                }
            }
            return val;
        }
        if (e.kind == Expr::Kind::Borrow) {
            return emit(*e.left);
        }
        if (e.kind == Expr::Kind::Copy) {
            Type t = analyzer.typeOf(*e.left);
            if (t.kind == TypeKind::Text) {
                llvm::Value* srcSlot = address(*e.left);
                auto* copySlot = builder.CreateAlloca(loomType(t), nullptr, "text.copy");
                auto copyFn = module->getOrInsertFunction("loom_text_clone",
                    llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false));
                builder.CreateCall(copyFn, {copySlot, srcSlot});
                return builder.CreateLoad(loomType(t), copySlot);
            } else if (t.kind == TypeKind::Vector) {
                llvm::Value* srcSlot = address(*e.left);
                auto* copySlot = builder.CreateAlloca(loomType(t), nullptr, "vec.copy");
                auto copyFn = module->getOrInsertFunction("loom_vec_clone",
                    llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false));
                builder.CreateCall(copyFn, {copySlot, srcSlot});
                return builder.CreateLoad(loomType(t), copySlot);
            } else if (t.kind == TypeKind::Pointer) {
                llvm::Value* srcPtr = emit(*e.left);
                auto copyFn = module->getOrInsertFunction("loom_ptr_clone",
                    llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy()}, false));
                return builder.CreateCall(copyFn, {srcPtr}, "ptr.copy");
            } else if (t.kind == TypeKind::SmartPointer) {
                return emit(*e.left);
            }
            return emit(*e.left);
        }
        if (e.kind == Expr::Kind::Allocate) {
            Type ptrType = analyzer.typeOf(e);
            Type elemType = *ptrType.pointeeType;
            llvm::Type* llvmElemType = loomType(elemType);
            const auto& dl = module->getDataLayout();
            uint64_t elemSize = dl.getTypeAllocSize(llvmElemType);
            uint64_t elemAlign = dl.getABITypeAlign(llvmElemType).value();
            if (elemAlign < 8) elemAlign = 8;

            llvm::Value* countVal = emit(*e.arguments[0]);
            if (countVal->getType() != builder.getInt64Ty()) {
                countVal = builder.CreateZExtOrTrunc(countVal, builder.getInt64Ty());
            }

            llvm::Value* mulRes = builder.CreateBinaryIntrinsic(llvm::Intrinsic::umul_with_overflow, countVal, builder.getInt64(elemSize), nullptr, "umul");
            llvm::Value* overflow = builder.CreateExtractValue(mulRes, 1, "mul_overflow");

            if (analyzer.typeOf(*e.arguments[0]).kind == TypeKind::Signed) {
                llvm::Value* isNeg = builder.CreateICmpSLT(countVal, builder.getInt64(0), "count.isneg");
                overflow = builder.CreateOr(overflow, isNeg, "alloc.overflow");
            }

            auto* curBB = builder.GetInsertBlock();
            auto* okBB = llvm::BasicBlock::Create(context, "alloc.ok", current);
            auto* doneBB = llvm::BasicBlock::Create(context, "alloc.done", current);

            builder.CreateCondBr(overflow, doneBB, okBB);

            builder.SetInsertPoint(okBB);
            llvm::Value* rawPtr = builder.CreateCall(getLoomAlloc(), {countVal, builder.getInt32(elemSize), builder.getInt32(elemAlign)}, "alloc_raw");
            builder.CreateBr(doneBB);
            auto* okEndBB = builder.GetInsertBlock();

            builder.SetInsertPoint(doneBB);
            auto* phi = builder.CreatePHI(builder.getPtrTy(), 2, "alloc.ptr");
            phi->addIncoming(llvm::ConstantPointerNull::get(builder.getPtrTy()), curBB);
            phi->addIncoming(rawPtr, okEndBB);
            return phi;
        }
        if (e.kind == Expr::Kind::Resize) {
            Type ptrType = analyzer.typeOf(e);
            Type elemType = *ptrType.pointeeType;
            llvm::Type* llvmElemType = loomType(elemType);
            const auto& dl = module->getDataLayout();
            uint64_t elemSize = dl.getTypeAllocSize(llvmElemType);
            uint64_t elemAlign = dl.getABITypeAlign(llvmElemType).value();
            if (elemAlign < 8) elemAlign = 8;

            llvm::Value* origPtr = emit(*e.arguments[0]);
            llvm::Value* countVal = emit(*e.arguments[1]);
            if (countVal->getType() != builder.getInt64Ty()) {
                countVal = builder.CreateZExtOrTrunc(countVal, builder.getInt64Ty());
            }

            return builder.CreateCall(getLoomRealloc(), {origPtr, countVal, builder.getInt32(elemSize), builder.getInt32(elemAlign)}, "realloc_ptr");
        }
        if (e.kind == Expr::Kind::Closure) {
            auto* savedInsertBlock = builder.GetInsertBlock();
            auto* savedCurrent = current;
            auto savedStorage = storage;
            auto savedTypes = types;

            std::vector<llvm::Type*> paramLlvmTypes;
            for (const auto& p : e.closureParams) {
                paramLlvmTypes.push_back(loomType(p.second));
            }
            Type retType = e.closureReturnType.kind != TypeKind::Invalid ? e.closureReturnType : Type::nullType();
            llvm::Type* retLlvmType = retType.kind == TypeKind::NullType ? builder.getVoidTy() : loomType(retType);
            auto* fnType = llvm::FunctionType::get(retLlvmType, paramLlvmTypes, false);
            static unsigned closureCounter = 0;
            std::string closureFnName = "__loom_closure_" + std::to_string(++closureCounter);
            auto* closureFn = llvm::Function::Create(fnType, llvm::Function::InternalLinkage, closureFnName, *module);
            current = closureFn;
            auto* entry = llvm::BasicBlock::Create(context, "entry", closureFn);
            builder.SetInsertPoint(entry);
            reachable = true;

            unsigned argIdx = 0;
            for (auto& arg : closureFn->args()) {
                const auto& p = e.closureParams[argIdx++];
                arg.setName(p.first);
                types.emplace(p.first, p.second);
                auto* slot = builder.CreateAlloca(loomType(p.second), nullptr, p.first + ".addr");
                builder.CreateStore(&arg, slot);
                storage.emplace(p.first, slot);
            }
            llvm::Value* bodyVal = emit(*e.closureBodyExpr);
            if (retType.kind == TypeKind::NullType || retLlvmType->isVoidTy()) {
                builder.CreateRetVoid();
            } else {
                builder.CreateRet(bodyVal);
            }

            storage = std::move(savedStorage);
            types = std::move(savedTypes);
            current = savedCurrent;
            builder.SetInsertPoint(savedInsertBlock);
            reachable = true;
            return closureFn;
        }
        if (e.kind == Expr::Kind::Release) {
            Type ptrType = analyzer.typeOf(*e.arguments[0]);
            if (ptrType.kind == TypeKind::Pointer && ptrType.pointeeType && ptrType.pointeeType->kind == TypeKind::Record) {
                std::string dropFnName = "__loom_drop_" + ptrType.pointeeType->name;
                auto* dropFn = module->getFunction(dropFnName);
                if (dropFn) {
                    llvm::Value* ptr = emit(*e.arguments[0]);
                    builder.CreateCall(dropFn, {ptr});
                    builder.CreateCall(getLoomFree(), {ptr});
                    return llvm::ConstantPointerNull::get(builder.getPtrTy());
                }
            }
            llvm::Value* ptr = emit(*e.arguments[0]);
            builder.CreateCall(getLoomFree(), {ptr});
            return llvm::ConstantPointerNull::get(builder.getPtrTy());
        }
        if (e.kind == Expr::Kind::Spawn) {
            const auto& call = *e.arguments[0];
            const Signature* sig = analyzer.signatureOf(call.text);
            if (!sig) throw Error("internal error: missing signature for spawned procedure '" + call.text + "'");

            llvm::Function* tramp = getOrCreateTaskTrampoline(call.text, *sig);
            const auto& dl = module->getDataLayout();

            llvm::Value* argsPtr = nullptr;
            uint64_t argsSize = 0;
            if (!sig->parameters.empty()) {
                std::vector<llvm::Type*> fieldTypes;
                for (const auto& pt : sig->parameters) {
                    fieldTypes.push_back(loomType(pt));
                }
                auto* argsStructType = llvm::StructType::get(context, fieldTypes);
                argsSize = dl.getTypeAllocSize(argsStructType);
                argsPtr = builder.CreateAlloca(argsStructType, nullptr, "task.args");
                for (unsigned i = 0; i < call.arguments.size(); ++i) {
                    llvm::Value* argVal = emit(*call.arguments[i]);
                    auto* fieldPtr = builder.CreateStructGEP(argsStructType, argsPtr, i, "task.arg." + std::to_string(i));
                    builder.CreateStore(argVal, fieldPtr);
                }
            } else {
                argsPtr = llvm::ConstantPointerNull::get(builder.getPtrTy());
            }

            uint64_t resSize = 0;
            if (sig->result && sig->result->kind != TypeKind::NullType) {
                llvm::Type* llvmResType = loomType(*sig->result);
                resSize = dl.getTypeAllocSize(llvmResType);
            }

            return builder.CreateCall(getLoomTaskSpawn(), {tramp, argsPtr, builder.getInt64(argsSize), builder.getInt64(resSize)}, "task_handle");
        }
        if (e.kind == Expr::Kind::Join) {
            llvm::Value* taskVal = emit(*e.arguments[0]);
            Type retType = analyzer.typeOf(e);
            if (e.arguments.size() == 2) {
                llvm::Value* timeoutVal = emit(*e.arguments[1]);
                if (timeoutVal->getType() != builder.getInt64Ty())
                    timeoutVal = builder.CreateZExtOrTrunc(timeoutVal, builder.getInt64Ty());
                auto* joinTimeoutFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty()}, false);
                auto joinTimeoutCallee = module->getOrInsertFunction("loom_task_join_timeout", joinTimeoutFnTy);
                if (retType.kind == TypeKind::NullType) {
                    llvm::Value* nullPtr = llvm::ConstantPointerNull::get(builder.getPtrTy());
                    return builder.CreateCall(joinTimeoutCallee, {taskVal, nullPtr, timeoutVal}, "join_timeout");
                } else {
                    llvm::Type* llvmResType = loomType(retType);
                    llvm::Value* resSlot = builder.CreateAlloca(llvmResType, nullptr, "task.res_slot");
                    builder.CreateCall(joinTimeoutCallee, {taskVal, resSlot, timeoutVal});
                    return builder.CreateLoad(llvmResType, resSlot, "task.result");
                }
            } else {
                if (retType.kind == TypeKind::NullType) {
                    builder.CreateCall(getLoomTaskJoin(), {taskVal, llvm::ConstantPointerNull::get(builder.getPtrTy())});
                    return llvm::ConstantPointerNull::get(builder.getPtrTy());
                } else {
                    llvm::Type* llvmResType = loomType(retType);
                    llvm::Value* resSlot = builder.CreateAlloca(llvmResType, nullptr, "task.res_slot");
                    builder.CreateCall(getLoomTaskJoin(), {taskVal, resSlot});
                    return builder.CreateLoad(llvmResType, resSlot, "task.result");
                }
            }
        }
        if (e.kind == Expr::Kind::Await) {
            llvm::Value* taskVal = emit(*e.left);
            Type retType = analyzer.typeOf(e);
            if (retType.kind == TypeKind::NullType) {
                builder.CreateCall(getLoomTaskJoin(), {taskVal, llvm::ConstantPointerNull::get(builder.getPtrTy())});
                return llvm::ConstantPointerNull::get(builder.getPtrTy());
            } else {
                llvm::Type* llvmResType = loomType(retType);
                llvm::Value* resSlot = builder.CreateAlloca(llvmResType, nullptr, "await.res_slot");
                builder.CreateCall(getLoomTaskJoin(), {taskVal, resSlot});
                return builder.CreateLoad(llvmResType, resSlot, "await.result");
            }
        }
        if (e.kind == Expr::Kind::RecordLiteral) {
            Type recType = analyzer.typeOf(e);
            llvm::Type* llvmRecType = loomType(recType);
            llvm::Value* recVal = llvm::UndefValue::get(llvmRecType);
            for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                unsigned fieldIdx = static_cast<unsigned>(analyzer.fieldIndex(recType.name, e.fieldNames[i]));
                llvm::Value* fieldVal = emit(*e.arguments[i]);
                recVal = builder.CreateInsertValue(recVal, fieldVal, fieldIdx, "rec.field");
            }
            return recVal;
        }
        if (e.kind == Expr::Kind::ArrayLiteral) {
            Type litType = analyzer.typeOf(e);
            if (litType.kind == TypeKind::Vector) {
                Type elemType = *litType.pointeeType;
                llvm::Type* llvmElemType = loomType(elemType);
                const auto& layout = module->getDataLayout();
                uint64_t elemSize = layout.getTypeAllocSize(llvmElemType);
                uint64_t elemAlign = layout.getABITypeAlign(llvmElemType).value();
                uint64_t count = e.arguments.size();
                auto* tempSlot = builder.CreateAlloca(loomType(litType), nullptr, "vec.lit.slot");
                auto initFn = module->getOrInsertFunction("loom_vec_init",
                    llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt32Ty(), builder.getInt32Ty()}, false));
                builder.CreateCall(initFn, {tempSlot, builder.getInt64(count), builder.getInt32(elemSize), builder.getInt32(elemAlign)});
                auto getFn = module->getOrInsertFunction("loom_vec_get",
                    llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false));
                for (unsigned i = 0; i < count; ++i) {
                    llvm::Value* elemAddr = builder.CreateCall(getFn, {tempSlot, builder.getInt64(i + 1)}, "vec.lit.elem.addr");
                    llvm::Value* val = emit(*e.arguments[i]);
                    builder.CreateStore(val, elemAddr);
                }
                return builder.CreateLoad(loomType(litType), tempSlot, "vec.lit");
            }
            llvm::Type* llvmArrType = loomType(litType);
            llvm::Value* arrVal = llvm::UndefValue::get(llvmArrType);
            for (unsigned i = 0; i < e.arguments.size(); ++i) {
                llvm::Value* elemVal = emit(*e.arguments[i]);
                arrVal = builder.CreateInsertValue(arrVal, elemVal, i, "arr.elem");
            }
            return arrVal;
        }
        if (e.kind == Expr::Kind::TupleLiteral) {
            Type tupType = analyzer.typeOf(e);
            llvm::Value* res = llvm::UndefValue::get(loomType(tupType));
            for (size_t i = 0; i < e.arguments.size(); ++i) {
                llvm::Value* elemVal = emit(*e.arguments[i]);
                res = builder.CreateInsertValue(res, elemVal, static_cast<unsigned>(i));
            }
            return res;
        }
        if (e.kind == Expr::Kind::HMapLiteral) {
            Type mapType = analyzer.typeOf(e);
            Type keyType = *mapType.keyType;
            Type valType = *mapType.valueType;
            uint32_t keyKind = (keyType.kind == TypeKind::String) ? 1 : 0;
            const auto& layout = module->getDataLayout();
            uint32_t valSize = layout.getTypeAllocSize(loomType(valType));
            uint32_t valAlign = layout.getABITypeAlign(loomType(valType)).value();

            auto* createFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty()}, false);
            auto createCallee = module->getOrInsertFunction("loom_hmap_create", createFnTy);
            llvm::Value* mapPtr = builder.CreateCall(createCallee, {builder.getInt32(keyKind), builder.getInt32(valSize), builder.getInt32(valAlign)}, "hmap");

            for (size_t i = 0; i < e.arguments.size(); i += 2) {
                llvm::Value* kVal = emit(*e.arguments[i]);
                llvm::Value* vVal = emit(*e.arguments[i + 1]);
                llvm::Value* valSlot = builder.CreateAlloca(loomType(valType));
                builder.CreateStore(vVal, valSlot);
                if (keyKind == 1) {
                    auto* insFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto insCallee = module->getOrInsertFunction("loom_hmap_insert_str", insFnTy);
                    builder.CreateCall(insCallee, {mapPtr, kVal, valSlot});
                } else {
                    if (kVal->getType() != builder.getInt64Ty())
                        kVal = builder.CreateZExtOrTrunc(kVal, builder.getInt64Ty());
                    auto* insFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getPtrTy()}, false);
                    auto insCallee = module->getOrInsertFunction("loom_hmap_insert_i64", insFnTy);
                    builder.CreateCall(insCallee, {mapPtr, kVal, valSlot});
                }
            }
            return mapPtr;
        }
        if (e.kind == Expr::Kind::Integer) {
            Type t = analyzer.typeOf(e);
            return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), e.magnitude);
        }
        if (e.kind == Expr::Kind::Boolean)
            return llvm::ConstantInt::get(loomType(Type::boolean()), e.boolean);
        if (e.kind == Expr::Kind::Floating) {
            Type t = analyzer.typeOf(e);
            return llvm::ConstantFP::get(loomType(t), e.floating);
        }
        if (e.kind == Expr::Kind::String) {
            auto* data = llvm::ConstantDataArray::getString(context, e.text, true);
            auto* global = new llvm::GlobalVariable(*module, data->getType(), true,
                llvm::GlobalValue::PrivateLinkage, data, ".loom.string." + std::to_string(++stringCounter));
            return builder.CreateInBoundsGEP(data->getType(), global,
                {builder.getInt32(0), builder.getInt32(0)}, "string");
        }
        if (e.kind == Expr::Kind::Character)
            return llvm::ConstantInt::get(loomType(Type::character()), decodeUtf8Scalar(e.text, e.line));
        if (e.kind == Expr::Kind::NullLiteral) {
            return llvm::ConstantPointerNull::get(builder.getPtrTy());
        }
        if (e.kind == Expr::Kind::Cast) {
            llvm::Value* val = emit(*e.left);
            Type srcType = analyzer.typeOf(*e.left);
            if (e.targetType.kind == TypeKind::Character && srcType.kind != TypeKind::Character) {
                llvm::Value* wide = emitCast(val, srcType, Type::unsignedInt(64));
                llvm::Value* inRange = builder.CreateICmpULE(wide, builder.getInt64(0x10FFFF), "character.range");
                llvm::Value* belowSurrogates = builder.CreateICmpULT(wide, builder.getInt64(0xD800), "character.low");
                llvm::Value* aboveSurrogates = builder.CreateICmpUGT(wide, builder.getInt64(0xDFFF), "character.high");
                llvm::Value* scalar = builder.CreateAnd(inRange, builder.CreateOr(belowSurrogates, aboveSurrogates), "character.scalar");
                auto* trapBB = llvm::BasicBlock::Create(context, "character.trap", current);
                auto* validBB = llvm::BasicBlock::Create(context, "character.valid", current);
                builder.CreateCondBr(scalar, validBB, trapBB);
                builder.SetInsertPoint(trapBB);
                auto* panicFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy()}, false);
                auto panicCallee = module->getOrInsertFunction("loom_panic", panicFnTy);
                builder.CreateCall(panicCallee, {getStringLiteral(sourcePath), builder.getInt32(e.line), getStringLiteral("invalid unicode codepoint for character")});
                builder.CreateUnreachable();
                builder.SetInsertPoint(validBB);
            }
            return emitCast(val, srcType, e.targetType);
        }
        if (e.kind == Expr::Kind::Index) {
            Type containerType = analyzer.typeOf(*e.left);
            if (containerType.kind == TypeKind::HMap) {
                llvm::Value* mapVal = emit(*e.left);
                llvm::Value* keyVal = emit(*e.right);
                Type keyType = *containerType.keyType;
                Type valType = *containerType.valueType;
                llvm::Value* resPtr = nullptr;
                if (keyType.kind == TypeKind::String) {
                    auto* getFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto getCallee = module->getOrInsertFunction("loom_hmap_get_str", getFnTy);
                    resPtr = builder.CreateCall(getCallee, {mapVal, keyVal}, "hmap.get");
                } else {
                    if (keyVal->getType() != builder.getInt64Ty())
                        keyVal = builder.CreateZExtOrTrunc(keyVal, builder.getInt64Ty());
                    auto* getFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                    auto getCallee = module->getOrInsertFunction("loom_hmap_get_i64", getFnTy);
                    resPtr = builder.CreateCall(getCallee, {mapVal, keyVal}, "hmap.get");
                }
                auto* isNull = builder.CreateICmpEQ(resPtr, llvm::ConstantPointerNull::get(builder.getPtrTy()), "hmap.check");
                auto* failBB = llvm::BasicBlock::Create(context, "hmap.fail", current);
                auto* contBB = llvm::BasicBlock::Create(context, "hmap.ok", current);
                builder.CreateCondBr(isNull, failBB, contBB);

                builder.SetInsertPoint(failBB);
                auto* panicFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy()}, false);
                auto panicCallee = module->getOrInsertFunction("loom_panic", panicFnTy);
                builder.CreateCall(panicCallee, {getStringLiteral(sourcePath), builder.getInt32(e.line), getStringLiteral("key not found in HMap")});
                builder.CreateUnreachable();

                builder.SetInsertPoint(contBB);
                return builder.CreateLoad(loomType(valType), resPtr, "hmap.val");
            }
            if (containerType.kind == TypeKind::String) {
                llvm::Value* byteAddr = address(e);
                return builder.CreateLoad(builder.getInt8Ty(), byteAddr, "str.byte");
            }
            Type elemType = analyzer.typeOf(e);
            llvm::Value* elemAddr = address(e);
            return builder.CreateLoad(loomType(elemType), elemAddr, "indexed");
        }
        if (e.kind == Expr::Kind::SliceConstructor) {
            if (!e.right) {
                Type arrType = analyzer.typeOf(*e.left);
                llvm::Value* arrAddr = address(*e.left);
                llvm::Value* zero = builder.getInt64(0);
                llvm::Value* dataPtr = builder.CreateInBoundsGEP(loomType(arrType), arrAddr, {zero, zero}, "slice.data");
                llvm::Value* lenVal = builder.getInt64(arrType.arraySize);
                llvm::Value* sliceVal = llvm::UndefValue::get(loomType(analyzer.typeOf(e)));
                sliceVal = builder.CreateInsertValue(sliceVal, dataPtr, 0);
                sliceVal = builder.CreateInsertValue(sliceVal, lenVal, 1);
                return sliceVal;
            } else {
                llvm::Value* ptrVal = emit(*e.left);
                Type lengthType = analyzer.typeOf(*e.right);
                llvm::Value* lenVal = emit(*e.right);
                if (lengthType.kind == TypeKind::Signed) {
                    auto* nonNegative = builder.CreateICmpSGE(lenVal, llvm::ConstantInt::get(lenVal->getType(), 0), "slice.length.nonnegative");
                    auto* okBB = llvm::BasicBlock::Create(context, "slice.length.ok", current);
                    auto* trapBB = llvm::BasicBlock::Create(context, "slice.length.trap", current);
                    builder.CreateCondBr(nonNegative, okBB, trapBB);
                    builder.SetInsertPoint(trapBB);
                    auto* panicFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy()}, false);
                    auto panicCallee = module->getOrInsertFunction("loom_panic", panicFnTy);
                    builder.CreateCall(panicCallee, {getStringLiteral(sourcePath), builder.getInt32(e.line), getStringLiteral("slice length cannot be negative")});
                    builder.CreateUnreachable();
                    builder.SetInsertPoint(okBB);
                    if (lenVal->getType() != builder.getInt64Ty())
                        lenVal = builder.CreateSExtOrTrunc(lenVal, builder.getInt64Ty());
                } else if (lenVal->getType() != builder.getInt64Ty()) {
                    lenVal = builder.CreateZExtOrTrunc(lenVal, builder.getInt64Ty());
                }
                auto* isNull = builder.CreateIsNull(ptrVal, "slice.pointer.null");
                auto* isEmpty = builder.CreateICmpEQ(lenVal, builder.getInt64(0), "slice.length.empty");
                auto* valid = builder.CreateOr(builder.CreateNot(isNull), isEmpty, "slice.pointer.valid");
                auto* okBB = llvm::BasicBlock::Create(context, "slice.valid", current);
                auto* trapBB = llvm::BasicBlock::Create(context, "slice.invalid", current);
                builder.CreateCondBr(valid, okBB, trapBB);
                builder.SetInsertPoint(trapBB);
                auto* panicFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy()}, false);
                auto panicCallee = module->getOrInsertFunction("loom_panic", panicFnTy);
                builder.CreateCall(panicCallee, {getStringLiteral(sourcePath), builder.getInt32(e.line), getStringLiteral("null pointer in slice construction")});
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBB);
                llvm::Value* sliceVal = llvm::UndefValue::get(loomType(analyzer.typeOf(e)));
                sliceVal = builder.CreateInsertValue(sliceVal, ptrVal, 0);
                sliceVal = builder.CreateInsertValue(sliceVal, lenVal, 1);
                return sliceVal;
            }
        }
        if (e.kind == Expr::Kind::SubSlice) {
            Type containerType = analyzer.typeOf(*e.left);
            llvm::Value* startVal = emit(*e.arguments[0]);
            llvm::Value* endVal = emit(*e.arguments[1]);
            if (startVal->getType() != builder.getInt64Ty())
                startVal = builder.CreateZExtOrTrunc(startVal, builder.getInt64Ty());
            if (endVal->getType() != builder.getInt64Ty())
                endVal = builder.CreateZExtOrTrunc(endVal, builder.getInt64Ty());
            if (containerType.kind == TypeKind::Text) {
                llvm::Value* textSlot = address(*e.left);
                auto* subFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt64Ty()}, false);
                auto subCallee = module->getOrInsertFunction("loom_text_subslice", subFnTy);
                return builder.CreateCall(subCallee, {textSlot, startVal, endVal}, "text.subslice");
            }
            llvm::Value* startIdx0 = builder.CreateSub(startVal, builder.getInt64(1), "subslice.start0");
            llvm::Value* subLen = builder.CreateAdd(builder.CreateSub(endVal, startVal), builder.getInt64(1), "subslice.len");
            Type elemType = *containerType.pointeeType;
            llvm::Value* subPtr = nullptr;
            if (containerType.kind == TypeKind::Array) {
                llvm::Value* arrAddr = address(*e.left);
                llvm::Value* zero = builder.getInt64(0);
                subPtr = builder.CreateInBoundsGEP(loomType(containerType), arrAddr, {zero, startIdx0}, "subslice.ptr");
            } else if (containerType.kind == TypeKind::Pointer) {
                llvm::Value* dataPtr = emit(*e.left);
                subPtr = builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {startIdx0}, "subslice.ptr");
            } else if (containerType.kind == TypeKind::Vector) {
                llvm::Value* vecAddr = address(*e.left);
                llvm::Value* dataPtrPtr = builder.CreateStructGEP(loomType(containerType), vecAddr, 0);
                llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(), dataPtrPtr, "vec.dataptr");
                subPtr = builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {startIdx0}, "subslice.ptr");
            } else {
                llvm::Value* srcSlice = emit(*e.left);
                llvm::Value* dataPtr = builder.CreateExtractValue(srcSlice, 0, "slice.dataptr");
                subPtr = builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {startIdx0}, "subslice.ptr");
            }
            llvm::Value* sliceVal = llvm::UndefValue::get(loomType(analyzer.typeOf(e)));
            sliceVal = builder.CreateInsertValue(sliceVal, subPtr, 0);
            sliceVal = builder.CreateInsertValue(sliceVal, subLen, 1);
            return sliceVal;
        }
        if (e.kind == Expr::Kind::Try) {
            llvm::Value* unionVal = emit(*e.left);
            llvm::Value* errCode = builder.CreateExtractValue(unionVal, 0, "try.err");
            llvm::Value* isErr = builder.CreateICmpNE(errCode, llvm::ConstantInt::get(errCode->getType(), 0), "try.is_err");
            auto* okBB = llvm::BasicBlock::Create(context, "try.ok", current);
            auto* errBB = llvm::BasicBlock::Create(context, "try.err_exit", current);
            builder.CreateCondBr(isErr, errBB, okBB);

            builder.SetInsertPoint(errBB);
            for (auto it = activeEnsures.rbegin(); it != activeEnsures.rend(); ++it) {
                emitEnsureStmt(**it);
            }
            llvm::Value* errRet = llvm::UndefValue::get(current->getReturnType());
            errRet = builder.CreateInsertValue(errRet, errCode, 0);
            builder.CreateRet(errRet);

            builder.SetInsertPoint(okBB);
            return builder.CreateExtractValue(unionVal, 1, "try.val");
        }
        if (e.kind == Expr::Kind::EnumVariant) {
            const auto value = analyzer.enumValueOf(e);
            const std::uint64_t bits = value.negative ? std::uint64_t(0) - value.magnitude : value.magnitude;
            return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(value.type)), bits);
        }
        if (e.kind == Expr::Kind::Address) {
            return address(*e.left);
        }
        if (e.kind == Expr::Kind::Deref) {
            llvm::Value* ptr = emit(*e.left);
            Type pointee = *analyzer.typeOf(*e.left).pointeeType;
            return builder.CreateLoad(loomType(pointee), ptr, pointee.isVolatile, "deref");
        }
        if (e.kind == Expr::Kind::Variable) {
            auto fit = functions.find(e.text);
            if (fit != functions.end()) return fit->second;
            return load(e.text);
        }
        if (e.kind == Expr::Kind::Member) {
            Type baseType = analyzer.typeOf(*e.left);
            if (baseType.kind == TypeKind::Array && e.text == "Length") {
                return builder.getInt64(baseType.arraySize);
            }
            if (baseType.kind == TypeKind::Slice && e.text == "Pointer") {
                llvm::Value* sliceVal = emit(*e.left);
                return builder.CreateExtractValue(sliceVal, 0, "slice.ptr");
            }
            if (baseType.kind == TypeKind::Slice && e.text == "Length") {
                llvm::Value* sliceVal = emit(*e.left);
                return builder.CreateExtractValue(sliceVal, 1, "slice.len");
            }
            if (baseType.kind == TypeKind::Vector && e.text == "Length") {
                llvm::Value* vecAddr = address(*e.left);
                auto* lenPtr = builder.CreateStructGEP(loomType(baseType), vecAddr, 1);
                return builder.CreateLoad(builder.getInt64Ty(), lenPtr, "vec.len");
            }
            if (baseType.kind == TypeKind::Vector && e.text == "Capacity") {
                llvm::Value* vecAddr = address(*e.left);
                auto* capPtr = builder.CreateStructGEP(loomType(baseType), vecAddr, 2);
                return builder.CreateLoad(builder.getInt64Ty(), capPtr, "vec.cap");
            }
            if (baseType.kind == TypeKind::Text && e.text == "Length") {
                llvm::Value* txtAddr = address(*e.left);
                auto* lenPtr = builder.CreateStructGEP(loomType(baseType), txtAddr, 1);
                return builder.CreateLoad(builder.getInt64Ty(), lenPtr, "text.len");
            }
            if (baseType.kind == TypeKind::Text && e.text == "Capacity") {
                llvm::Value* txtAddr = address(*e.left);
                auto* capPtr = builder.CreateStructGEP(loomType(baseType), txtAddr, 2);
                return builder.CreateLoad(builder.getInt64Ty(), capPtr, "text.cap");
            }
            if (baseType.kind == TypeKind::String && e.text == "Length") {
                llvm::Value* strVal = emit(*e.left);
                auto* isNull = builder.CreateICmpEQ(strVal, llvm::ConstantPointerNull::get(builder.getPtrTy()));
                auto* nullBB = llvm::BasicBlock::Create(context, "strlen.null", current);
                auto* notNullBB = llvm::BasicBlock::Create(context, "strlen.notnull", current);
                auto* contBB = llvm::BasicBlock::Create(context, "strlen.cont", current);
                builder.CreateCondBr(isNull, nullBB, notNullBB);

                builder.SetInsertPoint(nullBB);
                builder.CreateBr(contBB);

                builder.SetInsertPoint(notNullBB);
                auto* strlenType = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
                auto strlenCallee = module->getOrInsertFunction("strlen", strlenType);
                auto* lenVal = builder.CreateCall(strlenCallee, {strVal}, "str.len");
                builder.CreateBr(contBB);

                builder.SetInsertPoint(contBB);
                auto* phi = builder.CreatePHI(builder.getInt64Ty(), 2, "strlen.safe");
                phi->addIncoming(builder.getInt64(0), nullBB);
                phi->addIncoming(lenVal, notNullBB);
                return phi;
            }
            if (baseType.kind == TypeKind::HMap && e.text == "Length") {
                llvm::Value* mapPtr = emit(*e.left);
                auto* lenFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
                auto lenCallee = module->getOrInsertFunction("loom_hmap_length", lenFnTy);
                return builder.CreateCall(lenCallee, {mapPtr}, "hmap.len");
            }
            if (e.left->kind == Expr::Kind::Variable || e.left->kind == Expr::Kind::Member || e.left->kind == Expr::Kind::Deref) {
                return builder.CreateLoad(loomType(analyzer.typeOf(e)), address(e), e.text);
            }
            Type record = analyzer.typeOf(*e.left);
            return builder.CreateExtractValue(emit(*e.left), analyzer.fieldIndex(record.name, e.text), e.text);
        }
        if (e.kind == Expr::Kind::Call) {
            if (constEvaluator && (releaseMode || (optReport && optReport->enabled))) {
                unsigned steps = 0;
                auto constVal = constEvaluator->evalExpr(e, {}, 0, steps);
                if (constVal.isKnown()) {
                    if (optReport && optReport->enabled) {
                        optReport->frontendFolds.push_back("Pure block call '" + e.text + "(...)' evaluated at compile time -> " + valToString(constVal));
                    }
                    if (constVal.kind == ConstKind::Integer) {
                        Type t = analyzer.typeOf(e);
                        const std::uint64_t bits = constVal.isNegative ? std::uint64_t(0) - constVal.intVal : constVal.intVal;
                        return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), bits);
                    }
                    if (constVal.kind == ConstKind::Boolean) {
                        return llvm::ConstantInt::get(loomType(Type::boolean()), constVal.boolVal ? 1 : 0);
                    }
                    if (constVal.kind == ConstKind::Floating) {
                        Type t = analyzer.typeOf(e);
                        return llvm::ConstantFP::get(loomType(t), constVal.floatVal);
                    }
                    if (constVal.kind == ConstKind::Character) {
                        return llvm::ConstantInt::get(loomType(Type::character()), constVal.intVal);
                    }
                }
            }
            if (isVolatileRead(e.text)) {
                llvm::Value* addrVal = emit(*e.arguments[0]);
                llvm::Value* ptr = builder.CreateIntToPtr(addrVal, builder.getPtrTy(), "mmio.ptr");
                if (e.text == "Vol_Read_8") return builder.CreateLoad(builder.getInt8Ty(), ptr, /*isVolatile=*/true, "mmio.val8");
                if (e.text == "Vol_Read_16") return builder.CreateLoad(builder.getInt16Ty(), ptr, /*isVolatile=*/true, "mmio.val16");
                if (e.text == "Vol_Read_32") return builder.CreateLoad(builder.getInt32Ty(), ptr, /*isVolatile=*/true, "mmio.val32");
                if (e.text == "Vol_Read_64") return builder.CreateLoad(builder.getInt64Ty(), ptr, /*isVolatile=*/true, "mmio.val64");
            }
            if (isVolatileWrite(e.text)) {
                llvm::Value* addrVal = emit(*e.arguments[0]);
                llvm::Value* val = emit(*e.arguments[1]);
                llvm::Value* ptr = builder.CreateIntToPtr(addrVal, builder.getPtrTy(), "mmio.ptr");
                if (e.text == "Vol_Write_8") {
                    if (val->getType() != builder.getInt8Ty()) val = builder.CreateTruncOrBitCast(val, builder.getInt8Ty());
                    return builder.CreateStore(val, ptr, /*isVolatile=*/true);
                }
                if (e.text == "Vol_Write_16") {
                    if (val->getType() != builder.getInt16Ty()) val = builder.CreateTruncOrBitCast(val, builder.getInt16Ty());
                    return builder.CreateStore(val, ptr, /*isVolatile=*/true);
                }
                if (e.text == "Vol_Write_32") {
                    if (val->getType() != builder.getInt32Ty()) val = builder.CreateTruncOrBitCast(val, builder.getInt32Ty());
                    return builder.CreateStore(val, ptr, /*isVolatile=*/true);
                }
                if (e.text == "Vol_Write_64") {
                    if (val->getType() != builder.getInt64Ty()) val = builder.CreateZExtOrBitCast(val, builder.getInt64Ty());
                    return builder.CreateStore(val, ptr, /*isVolatile=*/true);
                }
            }
            if (isCpuIntrinsic(e.text)) {
                if (e.text == "Nop") {
                    auto* asmFn = llvm::InlineAsm::get(llvm::FunctionType::get(builder.getVoidTy(), false), "nop", "", true);
                    return builder.CreateCall(asmFn, {});
                }
                if (e.text == "WaitForInterrupt") {
                    auto* asmFn = llvm::InlineAsm::get(llvm::FunctionType::get(builder.getVoidTy(), false), "wfi", "", true);
                    return builder.CreateCall(asmFn, {});
                }
                if (e.text == "MemoryBarrier") {
                    auto* asmFn = llvm::InlineAsm::get(llvm::FunctionType::get(builder.getVoidTy(), false), "dmb sy", "", true);
                    return builder.CreateCall(asmFn, {});
                }
            }
            if (isAIMLIntrinsic(e.text)) {
                if (e.text == "Vector_Dot") {
                    llvm::Value* aPtr = getFloatPointer(*e.arguments[0]);
                    llvm::Value* bPtr = getFloatPointer(*e.arguments[1]);
                    llvm::Value* nVal = emit(*e.arguments[2]);
                    if (nVal->getType() != builder.getInt32Ty()) nVal = builder.CreateTruncOrBitCast(nVal, builder.getInt32Ty());
                    llvm::FunctionType* sdotType = llvm::FunctionType::get(
                        builder.getFloatTy(),
                        {builder.getInt32Ty(), builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy(), builder.getInt32Ty()},
                        false
                    );
                    auto sdotFunc = module->getOrInsertFunction("cblas_sdot", sdotType);
                    return builder.CreateCall(sdotFunc, {nVal, aPtr, builder.getInt32(1), bPtr, builder.getInt32(1)}, "dot");
                }
                if (e.text == "MatMul") {
                    emitMatMul(e, 0);
                    return nullptr;
                }
                if (e.text == "MatMul_TransposeB") {
                    emitMatMul(e, 1);
                    return nullptr;
                }
                if (e.text == "MatMul_TransposeA") {
                    emitMatMul(e, 2);
                    return nullptr;
                }
                if (e.text == "MatMul_Add") {
                    emitMatMulAdd(e);
                    return nullptr;
                }
                if (e.text == "ReLU") {
                    emitReLU(e);
                    return nullptr;
                }
                if (e.text == "ReLU_Backward") {
                    emitReLUBackward(e);
                    return nullptr;
                }
                if (e.text == "GELU") {
                    emitGELU(e);
                    return nullptr;
                }
                if (e.text == "Softmax") {
                    emitSoftmax(e);
                    return nullptr;
                }
                if (e.text == "LayerNorm") {
                    emitLayerNorm(e);
                    return nullptr;
                }
                if (e.text == "Vector_Add" || e.text == "Vector_Sub" || e.text == "Vector_Mul" || e.text == "Vector_Scale") {
                    emitVectorOp(e, e.text);
                    return nullptr;
                }
                if (e.text == "Parallel_For") {
                    emitParallelFor(e);
                    return nullptr;
                }
            }
            if (functions.find(e.text) == functions.end() && storage.find(e.text) == storage.end() && globalStorage.find(e.text) == globalStorage.end() && isMathIntrinsic(e.text)) {
                if (e.text == "Sqrt") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, val);
                }
                if (e.text == "Sin") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::sin, val);
                }
                if (e.text == "Cos") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::cos, val);
                }
                if (e.text == "Exp") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::exp, val);
                }
                if (e.text == "Log") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::log, val);
                }
                if (e.text == "Floor") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::floor, val);
                }
                if (e.text == "Ceil") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, val);
                }
                if (e.text == "Round") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    return builder.CreateUnaryIntrinsic(llvm::Intrinsic::round, val);
                }
                if (e.text == "Tan") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    bool is64 = val->getType()->isDoubleTy();
                    auto* fnTy = llvm::FunctionType::get(val->getType(), {val->getType()}, false);
                    auto callee = module->getOrInsertFunction(is64 ? "tan" : "tanf", fnTy);
                    return builder.CreateCall(callee, {val});
                }
                if (e.text == "ASin") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    bool is64 = val->getType()->isDoubleTy();
                    auto* fnTy = llvm::FunctionType::get(val->getType(), {val->getType()}, false);
                    auto callee = module->getOrInsertFunction(is64 ? "asin" : "asinf", fnTy);
                    return builder.CreateCall(callee, {val});
                }
                if (e.text == "ACos") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    bool is64 = val->getType()->isDoubleTy();
                    auto* fnTy = llvm::FunctionType::get(val->getType(), {val->getType()}, false);
                    auto callee = module->getOrInsertFunction(is64 ? "acos" : "acosf", fnTy);
                    return builder.CreateCall(callee, {val});
                }
                if (e.text == "ATan") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    bool is64 = val->getType()->isDoubleTy();
                    auto* fnTy = llvm::FunctionType::get(val->getType(), {val->getType()}, false);
                    auto callee = module->getOrInsertFunction(is64 ? "atan" : "atanf", fnTy);
                    return builder.CreateCall(callee, {val});
                }
                if (e.text == "ATan2") {
                    llvm::Value* y = emit(*e.arguments[0]);
                    llvm::Value* x = emit(*e.arguments[1]);
                    bool is64 = y->getType()->isDoubleTy();
                    auto* fnTy = llvm::FunctionType::get(y->getType(), {y->getType(), x->getType()}, false);
                    auto callee = module->getOrInsertFunction(is64 ? "atan2" : "atan2f", fnTy);
                    return builder.CreateCall(callee, {y, x});
                }
                if (e.text == "Abs") {
                    llvm::Value* val = emit(*e.arguments[0]);
                    if (val->getType()->isFloatingPointTy()) {
                        return builder.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, val);
                    } else {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::abs, val, builder.getInt1(false));
                    }
                }
                if (e.text == "Min") {
                    llvm::Value* a = emit(*e.arguments[0]);
                    llvm::Value* b = emit(*e.arguments[1]);
                    Type t = analyzer.typeOf(*e.arguments[0]);
                    if (t.kind == TypeKind::Float) {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::minnum, a, b);
                    } else if (t.kind == TypeKind::Signed) {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::smin, a, b);
                    } else {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::umin, a, b);
                    }
                }
                if (e.text == "Max") {
                    llvm::Value* a = emit(*e.arguments[0]);
                    llvm::Value* b = emit(*e.arguments[1]);
                    Type t = analyzer.typeOf(*e.arguments[0]);
                    if (t.kind == TypeKind::Float) {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, a, b);
                    } else if (t.kind == TypeKind::Signed) {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::smax, a, b);
                    } else {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::umax, a, b);
                    }
                }
                if (e.text == "Pow") {
                    llvm::Value* baseVal = emit(*e.arguments[0]);
                    llvm::Value* expVal = emit(*e.arguments[1]);
                    Type t = analyzer.typeOf(*e.arguments[0]);
                    if (t.kind == TypeKind::Float) {
                        return builder.CreateBinaryIntrinsic(llvm::Intrinsic::pow, baseVal, expVal);
                    } else if (t.kind == TypeKind::Signed) {
                        auto* fnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt64Ty(), builder.getInt64Ty()}, false);
                        auto callee = module->getOrInsertFunction("loom_ipow_i64", fnTy);
                        llvm::Value* b64 = builder.CreateSExtOrTrunc(baseVal, builder.getInt64Ty());
                        llvm::Value* e64 = builder.CreateSExtOrTrunc(expVal, builder.getInt64Ty());
                        llvm::Value* res64 = builder.CreateCall(callee, {b64, e64});
                        return builder.CreateIntCast(res64, loomType(t), true);
                    } else {
                        auto* fnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt64Ty(), builder.getInt64Ty()}, false);
                        auto callee = module->getOrInsertFunction("loom_ipow_u64", fnTy);
                        llvm::Value* b64 = builder.CreateZExtOrTrunc(baseVal, builder.getInt64Ty());
                        llvm::Value* e64 = builder.CreateZExtOrTrunc(expVal, builder.getInt64Ty());
                        llvm::Value* res64 = builder.CreateCall(callee, {b64, e64});
                        return builder.CreateIntCast(res64, loomType(t), false);
                    }
                }
            }
            if (functions.find(e.text) == functions.end() && storage.find(e.text) == storage.end() && globalStorage.find(e.text) == globalStorage.end() && isFileIntrinsic(e.text)) {
                if (e.text == "Open_File") {
                    Type modeType = analyzer.typeOf(*e.arguments[1]);
                    llvm::Value* modeIntVal = nullptr;
                    if (modeType.kind == TypeKind::String) {
                        int m = 1;
                        if (e.arguments[1]->kind == Expr::Kind::String) {
                            const std::string& s = e.arguments[1]->text;
                            if (s == "w") m = 2;
                            else if (s == "a") m = 3;
                            else if (s == "r+") m = 4;
                            else if (s == "w+") m = 5;
                            else m = 1;
                        }
                        modeIntVal = builder.getInt32(m);
                    } else {
                        llvm::Value* rawMode = emit(*e.arguments[1]);
                        modeIntVal = builder.CreateIntCast(rawMode, builder.getInt32Ty(), false);
                    }
                    auto* openFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy()}, false);
                    auto openCallee = module->getOrInsertFunction("loom_file_open", openFnTy);
                    auto* errSlot = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "file.err");
                    builder.CreateStore(builder.getInt32(0), errSlot);
                    llvm::Value* pathVal = emit(*e.arguments[0]);
                    return builder.CreateCall(openCallee, {pathVal, modeIntVal, errSlot});
                }
                if (e.text == "Read_All") {
                    auto* readAllFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto readAllCallee = module->getOrInsertFunction("loom_file_read_all", readAllFnTy);
                    auto* errSlot = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "file.err");
                    builder.CreateStore(builder.getInt32(0), errSlot);
                    llvm::Value* pathVal = emit(*e.arguments[0]);
                    return builder.CreateCall(readAllCallee, {pathVal, errSlot});
                }
                if (e.text == "Read_File") {
                    auto* readFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty(), builder.getPtrTy()}, false);
                    auto readCallee = module->getOrInsertFunction("loom_file_read", readFnTy);
                    auto* errSlot = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "file.err");
                    builder.CreateStore(builder.getInt32(0), errSlot);
                    llvm::Value* handle = emit(*e.arguments[0]);
                    llvm::Value* buf = emit(*e.arguments[1]);
                    llvm::Value* cnt = emit(*e.arguments[2]);
                    return builder.CreateCall(readCallee, {handle, buf, builder.CreateZExtOrTrunc(cnt, builder.getInt64Ty()), errSlot});
                }
                if (e.text == "Close_File") {
                    auto* closeFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy()}, false);
                    auto closeCallee = module->getOrInsertFunction("loom_file_close", closeFnTy);
                    llvm::Value* handle = emit(*e.arguments[0]);
                    builder.CreateCall(closeCallee, {handle});
                    return llvm::ConstantPointerNull::get(builder.getPtrTy());
                }
            }
            if ((e.text == "Append" || e.text == "Push") && (e.left || !e.arguments.empty())) {
                llvm::Value* vecSlot = e.left ? address(*e.left) : address(*e.arguments[0]);
                Type vecType = analyzer.typeOf(e.left ? *e.left : *e.arguments[0]);
                Type elemType = *vecType.pointeeType;
                llvm::Type* llvmElemType = loomType(elemType);
                auto* lenPtr = builder.CreateStructGEP(loomType(vecType), vecSlot, 1);
                auto* capPtr = builder.CreateStructGEP(loomType(vecType), vecSlot, 2);
                llvm::Value* curLen = builder.CreateLoad(builder.getInt64Ty(), lenPtr);
                llvm::Value* curCap = builder.CreateLoad(builder.getInt64Ty(), capPtr);
                llvm::Value* needsGrow = builder.CreateICmpEQ(curLen, curCap);
                llvm::Function* parentFn = builder.GetInsertBlock()->getParent();
                llvm::BasicBlock* growBB = llvm::BasicBlock::Create(context, "vec.grow", parentFn);
                llvm::BasicBlock* contBB = llvm::BasicBlock::Create(context, "vec.cont", parentFn);
                builder.CreateCondBr(needsGrow, growBB, contBB);
                builder.SetInsertPoint(growBB);
                auto* growFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy()}, false);
                auto growCallee = module->getOrInsertFunction("loom_vec_grow", growFnTy);
                builder.CreateCall(growCallee, {vecSlot});
                builder.CreateBr(contBB);
                builder.SetInsertPoint(contBB);
                auto* dataPtrField = builder.CreateStructGEP(loomType(vecType), vecSlot, 0);
                llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(), dataPtrField);
                llvm::Value* freshLen = builder.CreateLoad(builder.getInt64Ty(), lenPtr);
                llvm::Value* elemSlot = builder.CreateInBoundsGEP(llvmElemType, dataPtr, {freshLen});
                const Expr* itemExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                llvm::Value* itemVal = emit(*itemExpr);
                builder.CreateStore(itemVal, elemSlot);
                builder.CreateStore(builder.CreateAdd(freshLen, builder.getInt64(1)), lenPtr);
                return llvm::ConstantPointerNull::get(builder.getPtrTy());
            }
            if (e.text == "Pop" && (e.left || !e.arguments.empty())) {
                llvm::Value* vecSlot = e.left ? address(*e.left) : address(*e.arguments[0]);
                Type vecType = analyzer.typeOf(e.left ? *e.left : *e.arguments[0]);
                Type elemType = *vecType.pointeeType;
                llvm::Type* llvmElemType = loomType(elemType);
                auto* lenPtr = builder.CreateStructGEP(loomType(vecType), vecSlot, 1);
                llvm::Value* curLen = builder.CreateLoad(builder.getInt64Ty(), lenPtr);
                llvm::Value* newLen = builder.CreateSub(curLen, builder.getInt64(1));
                builder.CreateStore(newLen, lenPtr);
                auto* dataPtrField = builder.CreateStructGEP(loomType(vecType), vecSlot, 0);
                llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(), dataPtrField);
                llvm::Value* elemSlot = builder.CreateInBoundsGEP(llvmElemType, dataPtr, {newLen});
                return builder.CreateLoad(llvmElemType, elemSlot, "vec.pop");
            }
            auto getBufInfo = [&](const Expr& coll, llvm::Value*& dataPtr, llvm::Value*& lenVal, uint32_t& elemSize, Type& elemType) {
                Type collType = analyzer.typeOf(coll);
                elemType = *collType.pointeeType;
                const auto& dl = module->getDataLayout();
                elemSize = static_cast<uint32_t>(dl.getTypeAllocSize(loomType(elemType)));
                if (collType.kind == TypeKind::Array) {
                    dataPtr = address(coll);
                    lenVal = builder.getInt64(collType.arraySize);
                } else if (collType.kind == TypeKind::Slice) {
                    llvm::Value* sl = emit(coll);
                    dataPtr = builder.CreateExtractValue(sl, 0, "slice.ptr");
                    lenVal = builder.CreateExtractValue(sl, 1, "slice.len");
                } else if (collType.kind == TypeKind::Vector) {
                    llvm::Value* slot = address(coll);
                    auto* dataField = builder.CreateStructGEP(loomType(collType), slot, 0);
                    dataPtr = builder.CreateLoad(builder.getPtrTy(), dataField, "vec.data");
                    auto* lenField = builder.CreateStructGEP(loomType(collType), slot, 1);
                    lenVal = builder.CreateLoad(builder.getInt64Ty(), lenField, "vec.len");
                }
            };
            if (e.text == "Clear" && (e.left || !e.arguments.empty())) {
                const Expr* collExpr = e.left ? e.left.get() : e.arguments[0].get();
                Type collType = analyzer.typeOf(*collExpr);
                if (collType.kind == TypeKind::Vector) {
                    llvm::Value* vecSlot = address(*collExpr);
                    auto* clearFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false);
                    auto clearCallee = module->getOrInsertFunction("loom_vec_clear", clearFnTy);
                    builder.CreateCall(clearCallee, {vecSlot});
                    return llvm::ConstantPointerNull::get(builder.getPtrTy());
                } else if (collType.kind == TypeKind::Text) {
                    llvm::Value* textSlot = address(*collExpr);
                    auto* clearFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false);
                    auto clearCallee = module->getOrInsertFunction("loom_text_clear", clearFnTy);
                    builder.CreateCall(clearCallee, {textSlot});
                    return llvm::ConstantPointerNull::get(builder.getPtrTy());
                } else if (collType.kind == TypeKind::HMap) {
                    llvm::Value* mapVal = emit(*collExpr);
                    auto* clearFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false);
                    auto clearCallee = module->getOrInsertFunction("loom_hmap_clear", clearFnTy);
                    builder.CreateCall(clearCallee, {mapVal});
                    return llvm::ConstantPointerNull::get(builder.getPtrTy());
                }
            }
            if (e.text == "Index" && (e.left || !e.arguments.empty())) {
                const Expr* collExpr = e.left ? e.left.get() : e.arguments[0].get();
                Type collType = analyzer.typeOf(*collExpr);
                if (collType.kind == TypeKind::Text) {
                    llvm::Value* textSlot = address(*collExpr);
                    const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                    llvm::Value* subStr = emit(*argExpr);
                    auto* idxFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto idxCallee = module->getOrInsertFunction("loom_text_index", idxFnTy);
                    return builder.CreateCall(idxCallee, {textSlot, subStr}, "text.idx");
                }
                llvm::Value* dataPtr = nullptr;
                llvm::Value* lenVal = nullptr;
                uint32_t elemSize = 0;
                Type elemType;
                getBufInfo(*collExpr, dataPtr, lenVal, elemSize, elemType);
                const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                llvm::Value* itemVal = emit(*argExpr);
                auto* tempSlot = builder.CreateAlloca(loomType(elemType), nullptr, "idx.target");
                builder.CreateStore(itemVal, tempSlot);
                auto* idxFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt32Ty(), builder.getPtrTy()}, false);
                auto idxCallee = module->getOrInsertFunction("loom_buf_index", idxFnTy);
                return builder.CreateCall(idxCallee, {dataPtr, lenVal, builder.getInt32(elemSize), tempSlot}, "buf.idx");
            }
            if (e.text == "Index_All" && (e.left || !e.arguments.empty())) {
                const Expr* collExpr = e.left ? e.left.get() : e.arguments[0].get();
                llvm::Value* dataPtr = nullptr;
                llvm::Value* lenVal = nullptr;
                uint32_t elemSize = 0;
                Type elemType;
                getBufInfo(*collExpr, dataPtr, lenVal, elemSize, elemType);
                const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                llvm::Value* itemVal = emit(*argExpr);
                auto* tempSlot = builder.CreateAlloca(loomType(elemType), nullptr, "idx.target");
                builder.CreateStore(itemVal, tempSlot);
                Type resVecType = Type::vectorType(Type::unsignedInt(64), 0, false);
                auto* resSlot = builder.CreateAlloca(loomType(resVecType), nullptr, "idx.all.res");
                auto* idxAllFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty(), builder.getInt32Ty(), builder.getPtrTy()}, false);
                auto idxAllCallee = module->getOrInsertFunction("loom_buf_index_all", idxAllFnTy);
                builder.CreateCall(idxAllCallee, {resSlot, dataPtr, lenVal, builder.getInt32(elemSize), tempSlot});
                return builder.CreateLoad(loomType(resVecType), resSlot, "buf.idx_all");
            }
            if (e.text == "Contains" && (e.left || !e.arguments.empty())) {
                const Expr* collExpr = e.left ? e.left.get() : e.arguments[0].get();
                Type containerType = analyzer.typeOf(*collExpr);
                if (containerType.kind == TypeKind::HMap) {
                    llvm::Value* mapVal = emit(*collExpr);
                    const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                    llvm::Value* keyVal = emit(*argExpr);
                    if (containerType.keyType->kind == TypeKind::String) {
                        auto* cFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                        auto cCallee = module->getOrInsertFunction("loom_hmap_contains_str", cFnTy);
                        llvm::Value* callRes = builder.CreateCall(cCallee, {mapVal, keyVal}, "hmap.contains");
                        return builder.CreateICmpNE(callRes, builder.getInt32(0), "contains.bool");
                    } else {
                        if (keyVal->getType() != builder.getInt64Ty())
                            keyVal = builder.CreateZExtOrTrunc(keyVal, builder.getInt64Ty());
                        auto* cFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                        auto cCallee = module->getOrInsertFunction("loom_hmap_contains_i64", cFnTy);
                        llvm::Value* callRes = builder.CreateCall(cCallee, {mapVal, keyVal}, "hmap.contains");
                        return builder.CreateICmpNE(callRes, builder.getInt32(0), "contains.bool");
                    }
                }
                if (containerType.kind == TypeKind::Text) {
                    llvm::Value* textSlot = address(*collExpr);
                    const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                    llvm::Value* subStr = emit(*argExpr);
                    auto* cFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto cCallee = module->getOrInsertFunction("loom_text_contains", cFnTy);
                    llvm::Value* callRes = builder.CreateCall(cCallee, {textSlot, subStr}, "text.contains");
                    return builder.CreateICmpNE(callRes, builder.getInt32(0), "contains.bool");
                }
                llvm::Value* dataPtr = nullptr;
                llvm::Value* lenVal = nullptr;
                uint32_t elemSize = 0;
                Type elemType;
                getBufInfo(*collExpr, dataPtr, lenVal, elemSize, elemType);
                const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                llvm::Value* itemVal = emit(*argExpr);
                auto* tempSlot = builder.CreateAlloca(loomType(elemType), nullptr, "contains.target");
                builder.CreateStore(itemVal, tempSlot);
                auto* containsFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt32Ty(), builder.getPtrTy()}, false);
                auto containsCallee = module->getOrInsertFunction("loom_buf_contains", containsFnTy);
                llvm::Value* callRes = builder.CreateCall(containsCallee, {dataPtr, lenVal, builder.getInt32(elemSize), tempSlot}, "buf.contains");
                return builder.CreateICmpNE(callRes, builder.getInt32(0), "contains.bool");
            }
            if (e.text == "Length" && e.left && analyzer.typeOf(*e.left).kind == TypeKind::Vector) {
                llvm::Value* vecSlot = address(*e.left);
                Type vecType = analyzer.typeOf(*e.left);
                auto* lenPtr = builder.CreateStructGEP(loomType(vecType), vecSlot, 1);
                return builder.CreateLoad(builder.getInt64Ty(), lenPtr, "vec.len");
            }
            if (e.text == "Capacity" && e.left && analyzer.typeOf(*e.left).kind == TypeKind::Vector) {
                llvm::Value* vecSlot = address(*e.left);
                Type vecType = analyzer.typeOf(*e.left);
                auto* capPtr = builder.CreateStructGEP(loomType(vecType), vecSlot, 2);
                return builder.CreateLoad(builder.getInt64Ty(), capPtr, "vec.cap");
            }
            if (e.text == "Length" && e.left && analyzer.typeOf(*e.left).kind == TypeKind::HMap) {
                llvm::Value* mapVal = emit(*e.left);
                auto* lenFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
                auto lenCallee = module->getOrInsertFunction("loom_hmap_length", lenFnTy);
                return builder.CreateCall(lenCallee, {mapVal}, "hmap.len");
            }
            if (e.text == "Remove" && (e.left || !e.arguments.empty())) {
                Type containerType = analyzer.typeOf(e.left ? *e.left : *e.arguments[0]);
                if (containerType.kind == TypeKind::HMap) {
                    llvm::Value* mapVal = emit(e.left ? *e.left : *e.arguments[0]);
                    const Expr* argExpr = e.left ? e.arguments[0].get() : e.arguments[1].get();
                    llvm::Value* keyVal = emit(*argExpr);
                    if (containerType.keyType->kind == TypeKind::String) {
                        auto* rFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                        auto rCallee = module->getOrInsertFunction("loom_hmap_remove_str", rFnTy);
                        llvm::Value* callRes = builder.CreateCall(rCallee, {mapVal, keyVal}, "hmap.remove");
                        return builder.CreateICmpNE(callRes, builder.getInt32(0), "remove.bool");
                    } else {
                        if (keyVal->getType() != builder.getInt64Ty())
                            keyVal = builder.CreateZExtOrTrunc(keyVal, builder.getInt64Ty());
                        auto* rFnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                        auto rCallee = module->getOrInsertFunction("loom_hmap_remove_i64", rFnTy);
                        llvm::Value* callRes = builder.CreateCall(rCallee, {mapVal, keyVal}, "hmap.remove");
                        return builder.CreateICmpNE(callRes, builder.getInt32(0), "remove.bool");
                    }
                }
            }
            if (e.text == "To_String") {
                llvm::Value* textSlot = address(*e.arguments[0]);
                auto* toStrFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy()}, false);
                auto toStrCallee = module->getOrInsertFunction("loom_text_to_string", toStrFnTy);
                return builder.CreateCall(toStrCallee, {textSlot}, "to_string");
            }
            if (e.text == "__loom_task_cancel") {
                llvm::Value* taskVal = emit(*e.arguments[0]);
                auto* cancelFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false);
                auto cancelCallee = module->getOrInsertFunction("loom_task_cancel", cancelFnTy);
                builder.CreateCall(cancelCallee, {taskVal});
                return llvm::ConstantPointerNull::get(builder.getPtrTy());
            }
            if (e.text == "Make_Smt") {
                llvm::Value* val = emit(*e.arguments[0]);
                Type valType = analyzer.typeOf(*e.arguments[0]);
                llvm::Type* llvmValType = loomType(valType);
                const auto& dl = module->getDataLayout();
                uint64_t valSize = dl.getTypeAllocSize(llvmValType);
                auto* allocFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt64Ty(), builder.getPtrTy()}, false);
                auto allocCallee = module->getOrInsertFunction("loom_smt_alloc", allocFnTy);
                llvm::Value* dtor = llvm::ConstantPointerNull::get(builder.getPtrTy());
                llvm::Value* payloadPtr = builder.CreateCall(allocCallee, {builder.getInt64(valSize), dtor}, "smt.payload");
                builder.CreateStore(val, payloadPtr);
                return payloadPtr;
            }
            if (e.text == "Smt_Count") {
                llvm::Value* ptr = emit(*e.arguments[0]);
                auto* countFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
                auto countCallee = module->getOrInsertFunction("loom_smt_count", countFnTy);
                return builder.CreateCall(countCallee, {ptr}, "smt.count");
            }
            if (e.text == "Mutex_Create") {
                auto* fnTy = llvm::FunctionType::get(builder.getPtrTy(), {}, false);
                auto callee = module->getOrInsertFunction("loom_mutex_create", fnTy);
                return builder.CreateCall(callee, {}, "mutex");
            }
            if (e.text == "Channel_Create" || e.text == "Channel_Open") {
                llvm::Value* capVal = emit(*e.arguments[0]);
                if (capVal->getType() != builder.getInt64Ty()) capVal = builder.CreateZExtOrTrunc(capVal, builder.getInt64Ty());
                Type chType = analyzer.typeOf(e);
                Type elemType = *chType.pointeeType;
                const auto& dl = module->getDataLayout();
                uint32_t elemSize = static_cast<uint32_t>(dl.getTypeAllocSize(loomType(elemType)));
                auto* fnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt64Ty(), builder.getInt32Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_channel_create", fnTy);
                return builder.CreateCall(callee, {capVal, builder.getInt32(elemSize)}, "channel");
            }
            if (e.text == "Channel_Send") {
                llvm::Value* ch = emit(*e.arguments[0]);
                llvm::Value* val = emit(*e.arguments[1]);
                Type valType = analyzer.typeOf(*e.arguments[1]);
                llvm::Value* valSlot = builder.CreateAlloca(loomType(valType));
                builder.CreateStore(val, valSlot);
                auto* fnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                auto callee = module->getOrInsertFunction("loom_channel_send", fnTy);
                builder.CreateCall(callee, {ch, valSlot});
                return llvm::ConstantPointerNull::get(builder.getPtrTy());
            }
            if (e.text == "Channel_Receive") {
                llvm::Value* ch = emit(*e.arguments[0]);
                Type retType = analyzer.typeOf(e);
                llvm::Type* llvmRetType = loomType(retType);
                llvm::Value* outSlot = builder.CreateAlloca(llvmRetType);
                auto* fnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                auto callee = module->getOrInsertFunction("loom_channel_receive", fnTy);
                builder.CreateCall(callee, {ch, outSlot});
                return builder.CreateLoad(llvmRetType, outSlot, "channel.val");
            }
            if (e.text == "Channel_Try_Receive") {
                llvm::Value* ch = emit(*e.arguments[0]);
                Type retType = analyzer.typeOf(e);
                llvm::Type* llvmRetType = loomType(retType);
                llvm::Value* outSlot = builder.CreateAlloca(llvmRetType);
                auto* fnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                auto callee = module->getOrInsertFunction("loom_channel_try_receive", fnTy);
                builder.CreateCall(callee, {ch, outSlot});
                return builder.CreateLoad(llvmRetType, outSlot, "channel.val");
            }
            if (e.text == "Channel_Close") {
                llvm::Value* ch = emit(*e.arguments[0]);
                auto* fnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false);
                auto callee = module->getOrInsertFunction("loom_channel_close", fnTy);
                builder.CreateCall(callee, {ch});
                return llvm::ConstantPointerNull::get(builder.getPtrTy());
            }
            if (e.text == "Atomic_Load") {
                llvm::Value* ptr = address(*e.arguments[0]);
                Type valType = analyzer.typeOf(e);
                llvm::Type* llvmValType = loomType(valType);
                auto* loadInst = builder.CreateLoad(llvmValType, ptr, "atomic.load");
                loadInst->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
                const auto& dl = module->getDataLayout();
                loadInst->setAlignment(llvm::Align(dl.getTypeAllocSize(llvmValType)));
                return loadInst;
            }
            if (e.text == "Atomic_Store") {
                llvm::Value* ptr = address(*e.arguments[0]);
                llvm::Value* val = emit(*e.arguments[1]);
                auto* storeInst = builder.CreateStore(val, ptr);
                storeInst->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
                const auto& dl = module->getDataLayout();
                storeInst->setAlignment(llvm::Align(dl.getTypeAllocSize(val->getType())));
                return llvm::ConstantPointerNull::get(builder.getPtrTy());
            }
            if (e.text == "Atomic_Fetch_Add") {
                llvm::Value* ptr = address(*e.arguments[0]);
                llvm::Value* val = emit(*e.arguments[1]);
                const auto& dl = module->getDataLayout();
                return builder.CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::Add, ptr, val, llvm::MaybeAlign(dl.getTypeAllocSize(val->getType())), llvm::AtomicOrdering::SequentiallyConsistent);
            }
            if (e.text == "Atomic_Fetch_Sub") {
                llvm::Value* ptr = address(*e.arguments[0]);
                llvm::Value* val = emit(*e.arguments[1]);
                const auto& dl = module->getDataLayout();
                return builder.CreateAtomicRMW(llvm::AtomicRMWInst::BinOp::Sub, ptr, val, llvm::MaybeAlign(dl.getTypeAllocSize(val->getType())), llvm::AtomicOrdering::SequentiallyConsistent);
            }
            if (e.text == "Atomic_CAS") {
                llvm::Value* ptr = address(*e.arguments[0]);
                llvm::Value* expected = emit(*e.arguments[1]);
                llvm::Value* desired = emit(*e.arguments[2]);
                const auto& dl = module->getDataLayout();
                llvm::Value* pair = builder.CreateAtomicCmpXchg(ptr, expected, desired, llvm::MaybeAlign(dl.getTypeAllocSize(expected->getType())), llvm::AtomicOrdering::SequentiallyConsistent, llvm::AtomicOrdering::SequentiallyConsistent);
                return builder.CreateExtractValue(pair, 1, "cas.ok");
            }
            if (e.text == "Tcp_Listen") {
                llvm::Value* host = emit(*e.arguments[0]);
                if (analyzer.typeOf(*e.arguments[0]).kind == TypeKind::Text) {
                    auto* textDataField = builder.CreateStructGEP(loomType(analyzer.typeOf(*e.arguments[0])), address(*e.arguments[0]), 0);
                    host = builder.CreateLoad(builder.getPtrTy(), textDataField);
                }
                llvm::Value* port = emit(*e.arguments[1]);
                if (port->getType() != builder.getInt16Ty()) port = builder.CreateTruncOrBitCast(port, builder.getInt16Ty());
                auto* fnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getInt16Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_net_tcp_listen", fnTy);
                return builder.CreateCall(callee, {host, port}, "listen_fd");
            }
            if (e.text == "Tcp_Accept") {
                llvm::Value* fd = emit(*e.arguments[0]);
                if (fd->getType() != builder.getInt32Ty()) fd = builder.CreateTruncOrBitCast(fd, builder.getInt32Ty());
                auto* fnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getInt32Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_net_tcp_accept", fnTy);
                return builder.CreateCall(callee, {fd}, "client_fd");
            }
            if (e.text == "Tcp_Connect") {
                llvm::Value* host = emit(*e.arguments[0]);
                if (analyzer.typeOf(*e.arguments[0]).kind == TypeKind::Text) {
                    auto* textDataField = builder.CreateStructGEP(loomType(analyzer.typeOf(*e.arguments[0])), address(*e.arguments[0]), 0);
                    host = builder.CreateLoad(builder.getPtrTy(), textDataField);
                }
                llvm::Value* port = emit(*e.arguments[1]);
                if (port->getType() != builder.getInt16Ty()) port = builder.CreateTruncOrBitCast(port, builder.getInt16Ty());
                auto* fnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getInt16Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_net_tcp_connect", fnTy);
                return builder.CreateCall(callee, {host, port}, "conn_fd");
            }
            if (e.text == "Tcp_Send") {
                llvm::Value* fd = emit(*e.arguments[0]);
                if (fd->getType() != builder.getInt32Ty()) fd = builder.CreateTruncOrBitCast(fd, builder.getInt32Ty());
                llvm::Value* dataPtr = nullptr;
                llvm::Value* lenVal = nullptr;
                Type dataType = analyzer.typeOf(*e.arguments[1]);
                if (dataType.kind == TypeKind::Text) {
                    llvm::Value* tSlot = address(*e.arguments[1]);
                    auto* dField = builder.CreateStructGEP(loomType(dataType), tSlot, 0);
                    auto* lField = builder.CreateStructGEP(loomType(dataType), tSlot, 1);
                    dataPtr = builder.CreateLoad(builder.getPtrTy(), dField);
                    lenVal = builder.CreateLoad(builder.getInt64Ty(), lField);
                } else {
                    dataPtr = emit(*e.arguments[1]);
                    lenVal = builder.getInt64(64);
                }
                auto* fnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt32Ty(), builder.getPtrTy(), builder.getInt64Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_net_tcp_send", fnTy);
                return builder.CreateCall(callee, {fd, dataPtr, lenVal}, "sent_len");
            }
            if (e.text == "Tcp_Receive") {
                llvm::Value* fd = emit(*e.arguments[0]);
                if (fd->getType() != builder.getInt32Ty()) fd = builder.CreateTruncOrBitCast(fd, builder.getInt32Ty());
                llvm::Value* maxLen = emit(*e.arguments[1]);
                if (maxLen->getType() != builder.getInt64Ty()) maxLen = builder.CreateZExtOrTrunc(maxLen, builder.getInt64Ty());
                Type textType = Type::textType();
                llvm::Value* textSlot = builder.CreateAlloca(loomType(textType), nullptr, "tcp.recv.text");
                auto* initFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                auto initCallee = module->getOrInsertFunction("loom_text_init", initFnTy);
                builder.CreateCall(initCallee, {textSlot, maxLen});
                auto* dField = builder.CreateStructGEP(loomType(textType), textSlot, 0);
                llvm::Value* bufPtr = builder.CreateLoad(builder.getPtrTy(), dField);
                auto* recvFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt32Ty(), builder.getPtrTy(), builder.getInt64Ty()}, false);
                auto recvCallee = module->getOrInsertFunction("loom_net_tcp_receive", recvFnTy);
                llvm::Value* recvdBytes = builder.CreateCall(recvCallee, {fd, bufPtr, maxLen}, "recvd_len");
                auto* isNeg = builder.CreateICmpSLT(recvdBytes, builder.getInt64(0));
                auto* safeRecvd = builder.CreateSelect(isNeg, builder.getInt64(0), recvdBytes, "safe_recvd");
                auto* lField = builder.CreateStructGEP(loomType(textType), textSlot, 1);
                builder.CreateStore(safeRecvd, lField);
                return builder.CreateLoad(loomType(textType), textSlot, "tcp.text");
            }
            if (e.text == "Tcp_Close") {
                llvm::Value* fd = emit(*e.arguments[0]);
                if (fd->getType() != builder.getInt32Ty()) fd = builder.CreateTruncOrBitCast(fd, builder.getInt32Ty());
                auto* fnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getInt32Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_net_tcp_close", fnTy);
                return builder.CreateCall(callee, {fd}, "close_status");
            }
            if (e.text == "Udp_Bind") {
                llvm::Value* host = emit(*e.arguments[0]);
                if (analyzer.typeOf(*e.arguments[0]).kind == TypeKind::Text) {
                    auto* textDataField = builder.CreateStructGEP(loomType(analyzer.typeOf(*e.arguments[0])), address(*e.arguments[0]), 0);
                    host = builder.CreateLoad(builder.getPtrTy(), textDataField);
                }
                llvm::Value* port = emit(*e.arguments[1]);
                if (port->getType() != builder.getInt16Ty()) port = builder.CreateTruncOrBitCast(port, builder.getInt16Ty());
                auto* fnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getInt16Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_net_udp_bind", fnTy);
                return builder.CreateCall(callee, {host, port}, "udp_fd");
            }
            if (e.text == "Udp_Send_To") {
                llvm::Value* fd = emit(*e.arguments[0]);
                if (fd->getType() != builder.getInt32Ty()) fd = builder.CreateTruncOrBitCast(fd, builder.getInt32Ty());
                llvm::Value* host = emit(*e.arguments[1]);
                if (analyzer.typeOf(*e.arguments[1]).kind == TypeKind::Text) {
                    auto* textDataField = builder.CreateStructGEP(loomType(analyzer.typeOf(*e.arguments[1])), address(*e.arguments[1]), 0);
                    host = builder.CreateLoad(builder.getPtrTy(), textDataField);
                }
                llvm::Value* port = emit(*e.arguments[2]);
                if (port->getType() != builder.getInt16Ty()) port = builder.CreateTruncOrBitCast(port, builder.getInt16Ty());
                llvm::Value* dataPtr = nullptr;
                llvm::Value* lenVal = nullptr;
                Type dataType = analyzer.typeOf(*e.arguments[3]);
                if (dataType.kind == TypeKind::Text) {
                    llvm::Value* tSlot = address(*e.arguments[3]);
                    auto* dField = builder.CreateStructGEP(loomType(dataType), tSlot, 0);
                    auto* lField = builder.CreateStructGEP(loomType(dataType), tSlot, 1);
                    dataPtr = builder.CreateLoad(builder.getPtrTy(), dField);
                    lenVal = builder.CreateLoad(builder.getInt64Ty(), lField);
                } else {
                    dataPtr = emit(*e.arguments[3]);
                    lenVal = builder.getInt64(64);
                }
                auto* fnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt32Ty(), builder.getPtrTy(), builder.getInt16Ty(), builder.getPtrTy(), builder.getInt64Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_net_udp_send_to", fnTy);
                return builder.CreateCall(callee, {fd, host, port, dataPtr, lenVal}, "udp_sent");
            }
            if (e.text == "Udp_Receive_From") {
                llvm::Value* fd = emit(*e.arguments[0]);
                if (fd->getType() != builder.getInt32Ty()) fd = builder.CreateTruncOrBitCast(fd, builder.getInt32Ty());
                llvm::Value* maxLen = emit(*e.arguments[1]);
                if (maxLen->getType() != builder.getInt64Ty()) maxLen = builder.CreateZExtOrTrunc(maxLen, builder.getInt64Ty());
                Type textType = Type::textType();
                llvm::Value* textSlot = builder.CreateAlloca(loomType(textType), nullptr, "udp.recv.text");
                auto* initFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                auto initCallee = module->getOrInsertFunction("loom_text_init", initFnTy);
                builder.CreateCall(initCallee, {textSlot, maxLen});
                auto* dField = builder.CreateStructGEP(loomType(textType), textSlot, 0);
                llvm::Value* bufPtr = builder.CreateLoad(builder.getPtrTy(), dField);
                auto* recvFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt32Ty(), builder.getPtrTy(), builder.getInt64Ty()}, false);
                auto recvCallee = module->getOrInsertFunction("loom_net_udp_receive_from", recvFnTy);
                llvm::Value* recvdBytes = builder.CreateCall(recvCallee, {fd, bufPtr, maxLen}, "udp_recvd_len");
                auto* lField = builder.CreateStructGEP(loomType(textType), textSlot, 1);
                builder.CreateStore(recvdBytes, lField);
                return builder.CreateLoad(loomType(textType), textSlot, "udp.text");
            }
            if (e.text == "Udp_Close") {
                llvm::Value* fd = emit(*e.arguments[0]);
                if (fd->getType() != builder.getInt32Ty()) fd = builder.CreateTruncOrBitCast(fd, builder.getInt32Ty());
                auto* fnTy = llvm::FunctionType::get(builder.getInt32Ty(), {builder.getInt32Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_net_udp_close", fnTy);
                return builder.CreateCall(callee, {fd}, "close_status");
            }
            auto f = functions.find(e.text);
            if (f == functions.end()) {
                auto loc = storage.find(e.text);
                auto glob = globalStorage.find(e.text);
                if (loc != storage.end() || glob != globalStorage.end()) {
                    llvm::Value* varSlot = loc != storage.end() ? loc->second : glob->second;
                    llvm::Value* fnPtr = builder.CreateLoad(builder.getPtrTy(), varSlot, e.text + ".fnptr");
                    Type retType = analyzer.typeOf(e);
                    llvm::Type* retLlvmType = retType.kind == TypeKind::NullType ? builder.getVoidTy() : loomType(retType);
                    std::vector<llvm::Type*> paramLlvmTypes;
                    std::vector<llvm::Value*> args;
                    for (const auto& arg : e.arguments) {
                        args.push_back(emit(*arg));
                        paramLlvmTypes.push_back(loomType(analyzer.typeOf(*arg)));
                    }
                    auto* fnTy = llvm::FunctionType::get(retLlvmType, paramLlvmTypes, false);
                    return builder.CreateCall(fnTy, fnPtr, args, retLlvmType->isVoidTy() ? "" : "bp.call");
                }
                throw Error("internal error: missing function declaration '" + e.text + "'");
            }
            const auto* sig = analyzer.signatureOf(e.text);
            if (sig && sig->isAsync) {
                llvm::Function* tramp = getOrCreateTaskTrampoline(e.text, *sig);
                const auto& dl = module->getDataLayout();
                uint64_t argsSize = 0;
                llvm::Value* argsPtr = llvm::ConstantPointerNull::get(builder.getPtrTy());
                if (!sig->parameters.empty()) {
                    std::vector<llvm::Type*> fieldTypes;
                    for (const auto& pt : sig->parameters) fieldTypes.push_back(loomType(pt));
                    auto* argsStructType = llvm::StructType::get(context, fieldTypes);
                    argsSize = dl.getTypeAllocSize(argsStructType);
                    argsPtr = builder.CreateAlloca(argsStructType, nullptr, "task.args");
                    for (unsigned i = 0; i < e.arguments.size(); ++i) {
                        auto* fieldPtr = builder.CreateStructGEP(argsStructType, argsPtr, i);
                        builder.CreateStore(emit(*e.arguments[i]), fieldPtr);
                    }
                }
                uint64_t resSize = 0;
                if (sig->result && sig->result->kind != TypeKind::NullType) {
                    resSize = dl.getTypeAllocSize(loomType(*sig->result));
                }
                return builder.CreateCall(getLoomTaskSpawn(), {tramp, argsPtr, builder.getInt64(argsSize), builder.getInt64(resSize)}, "async.future");
            }
            std::vector<llvm::Value*> args;
            for (std::size_t i = 0; i < e.arguments.size(); ++i) {
                const auto& arg = e.arguments[i];
                bool isRefOrMut = false;
                auto fnIt = functionDecls.find(e.text);
                if (fnIt != functionDecls.end() && i < fnIt->second->params.size()) {
                    if (fnIt->second->params[i].isMut || fnIt->second->params[i].isRef) {
                        isRefOrMut = true;
                    }
                }
                if (isRefOrMut) {
                    args.push_back(address(*arg));
                } else {
                    args.push_back(emit(*arg));
                }
            }
            return builder.CreateCall(f->second, args, f->second->getReturnType()->isVoidTy() ? "" : "call");
        }
        if (e.kind == Expr::Kind::Unary) {
            if (constEvaluator) {
                unsigned steps = 0;
                auto constVal = constEvaluator->evalExpr(e, {}, 0, steps);
                if (constVal.isKnown()) {
                    if (optReport && optReport->enabled) {
                        optReport->frontendFolds.push_back("Constant unary op '" + e.text + "' evaluated at compile time -> " + valToString(constVal));
                    }
                    if (constVal.kind == ConstKind::Integer) {
                        Type t = analyzer.typeOf(e);
                        const std::uint64_t bits = constVal.isNegative ? std::uint64_t(0) - constVal.intVal : constVal.intVal;
                        return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), bits);
                    }
                    if (constVal.kind == ConstKind::Boolean) {
                        return llvm::ConstantInt::get(loomType(Type::boolean()), constVal.boolVal ? 1 : 0);
                    }
                    if (constVal.kind == ConstKind::Floating) {
                        Type t = analyzer.typeOf(e);
                        return llvm::ConstantFP::get(loomType(t), constVal.floatVal);
                    }
                }
            }
            auto* a = emit(*e.left);
            if (e.text == "-") {
                if (analyzer.typeOf(*e.left).kind == TypeKind::Float)
                    return builder.CreateFNeg(a, "fneg");
                Type t = analyzer.typeOf(*e.left);
                if (t.kind == TypeKind::Signed) {
                    llvm::Value* zero = llvm::ConstantInt::get(a->getType(), 0);
                    auto* resStruct = builder.CreateBinaryIntrinsic(llvm::Intrinsic::ssub_with_overflow, zero, a, nullptr, "checked.neg");
                    auto* resVal = builder.CreateExtractValue(resStruct, 0, "res");
                    auto* ovf = builder.CreateExtractValue(resStruct, 1, "ovf");
                    auto* failBlock = llvm::BasicBlock::Create(context, "neg.fail", current);
                    auto* okBlock = llvm::BasicBlock::Create(context, "neg.ok", current);
                    builder.CreateCondBr(ovf, failBlock, okBlock);

                    builder.SetInsertPoint(failBlock);
                    auto* panicFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto panicCallee = module->getOrInsertFunction("loom_panic_overflow", panicFnTy);
                    std::string typeName = t.name.empty() ? ("Signed_" + std::to_string(a->getType()->getIntegerBitWidth())) : t.name;
                    builder.CreateCall(panicCallee, {
                        getStringLiteral(sourcePath),
                        builder.getInt32(e.line),
                        getStringLiteral("negation"),
                        getStringLiteral(typeName)
                    });
                    builder.CreateUnreachable();

                    builder.SetInsertPoint(okBlock);
                    return resVal;
                }
                return builder.CreateNeg(a, "neg");
            }
            if (e.text == "~")
                return builder.CreateNot(a, "bnot");
            return builder.CreateNot(a, "not");
        }
        if (constEvaluator) {
            unsigned steps = 0;
            auto constVal = constEvaluator->evalExpr(e, {}, 0, steps);
            if (constVal.isKnown()) {
                if (optReport && optReport->enabled) {
                    optReport->frontendFolds.push_back("Constant binary op '" + e.text + "' evaluated at compile time -> " + valToString(constVal));
                }
                if (constVal.kind == ConstKind::Integer) {
                    Type t = analyzer.typeOf(e);
                    const std::uint64_t bits = constVal.isNegative ? std::uint64_t(0) - constVal.intVal : constVal.intVal;
                    return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), bits);
                }
                if (constVal.kind == ConstKind::Boolean) {
                    return llvm::ConstantInt::get(loomType(Type::boolean()), constVal.boolVal ? 1 : 0);
                }
                if (constVal.kind == ConstKind::Floating) {
                    Type t = analyzer.typeOf(e);
                    return llvm::ConstantFP::get(loomType(t), constVal.floatVal);
                }
                if (constVal.kind == ConstKind::Character) {
                    return llvm::ConstantInt::get(loomType(Type::character()), constVal.intVal);
                }
            }
        }
        const std::string op = lower(e.text);
        if (op == "and") {
            llvm::Value* lhs = emit(*e.left);
            auto* lhsBB = builder.GetInsertBlock();
            auto* rhsBB = llvm::BasicBlock::Create(context, "and.rhs", current);
            auto* mergeBB = llvm::BasicBlock::Create(context, "and.merge", current);
            builder.CreateCondBr(lhs, rhsBB, mergeBB);

            builder.SetInsertPoint(rhsBB);
            llvm::Value* rhs = emit(*e.right);
            auto* rhsEndBB = builder.GetInsertBlock();
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(mergeBB);
            auto* phi = builder.CreatePHI(builder.getInt1Ty(), 2, "and.res");
            phi->addIncoming(builder.getInt1(false), lhsBB);
            phi->addIncoming(rhs, rhsEndBB);
            return phi;
        }
        if (op == "or") {
            llvm::Value* lhs = emit(*e.left);
            auto* lhsBB = builder.GetInsertBlock();
            auto* rhsBB = llvm::BasicBlock::Create(context, "or.rhs", current);
            auto* mergeBB = llvm::BasicBlock::Create(context, "or.merge", current);
            builder.CreateCondBr(lhs, mergeBB, rhsBB);

            builder.SetInsertPoint(rhsBB);
            llvm::Value* rhs = emit(*e.right);
            auto* rhsEndBB = builder.GetInsertBlock();
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(mergeBB);
            auto* phi = builder.CreatePHI(builder.getInt1Ty(), 2, "or.res");
            phi->addIncoming(builder.getInt1(true), lhsBB);
            phi->addIncoming(rhs, rhsEndBB);
            return phi;
        }
        auto* a = emit(*e.left);
        auto* b = emit(*e.right);
        Type t = analyzer.typeOf(*e.left);
        if (op == "pow") {
            if (t.kind == TypeKind::Float) {
                return builder.CreateBinaryIntrinsic(llvm::Intrinsic::pow, a, b);
            } else if (t.kind == TypeKind::Signed) {
                auto* fnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt64Ty(), builder.getInt64Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_ipow_i64", fnTy);
                llvm::Value* b64 = builder.CreateSExtOrTrunc(a, builder.getInt64Ty());
                llvm::Value* e64 = builder.CreateSExtOrTrunc(b, builder.getInt64Ty());
                llvm::Value* res64 = builder.CreateCall(callee, {b64, e64});
                return builder.CreateIntCast(res64, loomType(t), true);
            } else {
                auto* fnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt64Ty(), builder.getInt64Ty()}, false);
                auto callee = module->getOrInsertFunction("loom_ipow_u64", fnTy);
                llvm::Value* b64 = builder.CreateZExtOrTrunc(a, builder.getInt64Ty());
                llvm::Value* e64 = builder.CreateZExtOrTrunc(b, builder.getInt64Ty());
                llvm::Value* res64 = builder.CreateCall(callee, {b64, e64});
                return builder.CreateIntCast(res64, loomType(t), false);
            }
        }
        if (op == "+") {
            if (t.kind == TypeKind::Text) {
                llvm::Value* textSlot = address(*e.left);
                Type rType = analyzer.typeOf(*e.right);
                if (rType.kind == TypeKind::String) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_str", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, b});
                } else if (rType.kind == TypeKind::Character) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt8Ty()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_char", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, builder.CreateIntCast(b, builder.getInt8Ty(), false)});
                } else if (rType.kind == TypeKind::Signed) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_i64", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, builder.CreateSExtOrTrunc(b, builder.getInt64Ty())});
                } else if (rType.kind == TypeKind::Unsigned) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_u64", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, builder.CreateZExtOrTrunc(b, builder.getInt64Ty())});
                } else if (rType.kind == TypeKind::Float) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getDoubleTy()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_f64", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, builder.CreateFPExt(b, builder.getDoubleTy())});
                } else if (rType.kind == TypeKind::Boolean) {
                    auto* appendFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt1Ty()}, false);
                    auto appendCallee = module->getOrInsertFunction("loom_text_append_bool", appendFnTy);
                    builder.CreateCall(appendCallee, {textSlot, b});
                }
                return builder.CreateLoad(loomType(t), textSlot);
            }
        }
        if (t.kind == TypeKind::Float) {
            builder.setFastMathFlags(llvm::FastMathFlags());
            if (op == "+") return builder.CreateFAdd(a, b, "fadd");
            if (op == "-") return builder.CreateFSub(a, b, "fsub");
            if (op == "*") return builder.CreateFMul(a, b, "fmul");
            if (op == "/") return builder.CreateFDiv(a, b, "fdiv");
            if (op == "==") return builder.CreateFCmpOEQ(a, b, "fcmp");
            if (op == "!=") return builder.CreateFCmpUNE(a, b, "fcmp");
            if (op == "<") return builder.CreateFCmpOLT(a, b, "fcmp");
            if (op == "<=") return builder.CreateFCmpOLE(a, b, "fcmp");
            if (op == ">") return builder.CreateFCmpOGT(a, b, "fcmp");
            return builder.CreateFCmpOGE(a, b, "fcmp");
        }
        if (op == "+" || op == "-" || op == "*") {
            if (t.kind == TypeKind::Signed || t.kind == TypeKind::Unsigned) {
                if (b->getType() != a->getType()) {
                    b = builder.CreateIntCast(b, a->getType(), t.kind == TypeKind::Signed);
                }
                llvm::Intrinsic::ID iid;
                if (op == "+") {
                    iid = (t.kind == TypeKind::Signed) ? llvm::Intrinsic::sadd_with_overflow : llvm::Intrinsic::uadd_with_overflow;
                } else if (op == "-") {
                    iid = (t.kind == TypeKind::Signed) ? llvm::Intrinsic::ssub_with_overflow : llvm::Intrinsic::usub_with_overflow;
                } else {
                    iid = (t.kind == TypeKind::Signed) ? llvm::Intrinsic::smul_with_overflow : llvm::Intrinsic::umul_with_overflow;
                }
                auto* resStruct = builder.CreateBinaryIntrinsic(iid, a, b, nullptr, "checked." + op);
                auto* resVal = builder.CreateExtractValue(resStruct, 0, "res");
                auto* ovf = builder.CreateExtractValue(resStruct, 1, "ovf");
                auto* failBlock = llvm::BasicBlock::Create(context, "ovf.fail", current);
                auto* okBlock = llvm::BasicBlock::Create(context, "ovf.ok", current);
                builder.CreateCondBr(ovf, failBlock, okBlock);

                builder.SetInsertPoint(failBlock);
                auto* panicFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy(), builder.getPtrTy()}, false);
                auto panicCallee = module->getOrInsertFunction("loom_panic_overflow", panicFnTy);
                std::string typeName = t.name.empty() ? ((t.kind == TypeKind::Signed ? "Signed_" : "Unsigned_") + std::to_string(a->getType()->getIntegerBitWidth())) : t.name;
                builder.CreateCall(panicCallee, {
                    getStringLiteral(sourcePath),
                    builder.getInt32(e.line),
                    getStringLiteral(op),
                    getStringLiteral(typeName)
                });
                builder.CreateUnreachable();

                builder.SetInsertPoint(okBlock);
                return resVal;
            }
            if (op == "+") return builder.CreateAdd(a, b, "add");
            if (op == "-") return builder.CreateSub(a, b, "sub");
            if (op == "*") return builder.CreateMul(a, b, "mul");
        }
        if (op == "/" || op == "mod" || op == "%") {
            if (t.kind == TypeKind::Unsigned || t.kind == TypeKind::Signed) {
                auto* isZero = builder.CreateICmpEQ(b, llvm::ConstantInt::get(b->getType(), 0), "divzero.cond");
                auto* failBlock = llvm::BasicBlock::Create(context, "divzero.fail", current);
                auto* okBlock = llvm::BasicBlock::Create(context, "divzero.ok", current);
                builder.CreateCondBr(isZero, failBlock, okBlock);
                builder.SetInsertPoint(failBlock);
                auto* divZeroFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt32Ty()}, false);
                auto divZeroCallee = module->getOrInsertFunction("loom_panic_divzero", divZeroFnTy);
                builder.CreateCall(divZeroCallee, {getStringLiteral(sourcePath), builder.getInt32(e.line)});
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBlock);

                if (t.kind == TypeKind::Signed && op == "/") {
                    unsigned bitWidth = a->getType()->getIntegerBitWidth();
                    llvm::APInt minVal = llvm::APInt::getSignedMinValue(bitWidth);
                    auto* isMin = builder.CreateICmpEQ(a, llvm::ConstantInt::get(context, minVal), "sdiv.ismin");
                    auto* isNegOne = builder.CreateICmpEQ(b, llvm::ConstantInt::get(a->getType(), -1), "sdiv.isnegone");
                    auto* sdivOvf = builder.CreateAnd(isMin, isNegOne, "sdiv.ovf");
                    auto* sdivFailBlock = llvm::BasicBlock::Create(context, "sdiv.ovf.fail", current);
                    auto* sdivOkBlock = llvm::BasicBlock::Create(context, "sdiv.ovf.ok", current);
                    builder.CreateCondBr(sdivOvf, sdivFailBlock, sdivOkBlock);

                    builder.SetInsertPoint(sdivFailBlock);
                    auto* panicFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy(), builder.getPtrTy()}, false);
                    auto panicCallee = module->getOrInsertFunction("loom_panic_overflow", panicFnTy);
                    std::string typeName = t.name.empty() ? ("Signed_" + std::to_string(bitWidth)) : t.name;
                    builder.CreateCall(panicCallee, {
                        getStringLiteral(sourcePath),
                        builder.getInt32(e.line),
                        getStringLiteral("/"),
                        getStringLiteral(typeName)
                    });
                    builder.CreateUnreachable();
                    builder.SetInsertPoint(sdivOkBlock);
                }
            }
            if (op == "/")
                return t.kind == TypeKind::Unsigned ? builder.CreateUDiv(a, b, "divu") : builder.CreateSDiv(a, b, "divs");
            return t.kind == TypeKind::Unsigned ? builder.CreateURem(a, b, "urem") : builder.CreateSRem(a, b, "srem");
        }
        if (op == "&")
            return builder.CreateAnd(a, b, "band");
        if (op == "|")
            return builder.CreateOr(a, b, "bor");
        if (op == "^")
            return builder.CreateXor(a, b, "bxor");
        if (op == "lsl" || op == "lsr") {
            unsigned bits = a->getType()->getIntegerBitWidth();
            llvm::Value* origB = b;
            if (origB->getType()->getIntegerBitWidth() < 64)
                origB = builder.CreateSExt(origB, builder.getInt64Ty());
            else if (origB->getType()->getIntegerBitWidth() > 64)
                origB = builder.CreateTrunc(origB, builder.getInt64Ty());
            llvm::Value* tooSmall = builder.CreateICmpSLT(origB, builder.getInt64(0), "shift.neg");
            llvm::Value* tooBig = builder.CreateICmpSGE(origB, builder.getInt64(bits), "shift.overflow");
            llvm::Value* badShift = builder.CreateOr(tooSmall, tooBig, "shift.bad");
            auto* failBlock = llvm::BasicBlock::Create(context, "shift.fail", current);
            auto* okBlock = llvm::BasicBlock::Create(context, "shift.ok", current);
            builder.CreateCondBr(badShift, failBlock, okBlock);
            builder.SetInsertPoint(failBlock);
            auto* panicFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt32Ty(), builder.getPtrTy()}, false);
            auto panicCallee = module->getOrInsertFunction("loom_panic", panicFnTy);
            builder.CreateCall(panicCallee, {getStringLiteral(sourcePath), builder.getInt32(e.line), getStringLiteral("shift count out of bounds")});
            builder.CreateUnreachable();
            builder.SetInsertPoint(okBlock);

            if (b->getType() != a->getType())
                b = builder.CreateZExtOrTrunc(b, a->getType());
            if (op == "lsl") return builder.CreateShl(a, b, "shl");
            return t.kind == TypeKind::Unsigned ? builder.CreateLShr(a, b, "lshr") : builder.CreateAShr(a, b, "ashr");
        }
        if (op == "and")
            return builder.CreateAnd(a, b, "and");
        if (op == "or")
            return builder.CreateOr(a, b, "or");
        if (op == "==")
            return builder.CreateICmpEQ(a, b, "eq");
        if (op == "!=")
            return builder.CreateICmpNE(a, b, "ne");
        if (t.kind == TypeKind::Unsigned || t.kind == TypeKind::Character) {
            if (op == "<")
                return builder.CreateICmpULT(a, b, "lt");
            if (op == "<=")
                return builder.CreateICmpULE(a, b, "le");
            if (op == ">")
                return builder.CreateICmpUGT(a, b, "gt");
            return builder.CreateICmpUGE(a, b, "ge");
        }
        if (op == "<")
            return builder.CreateICmpSLT(a, b, "lt");
        if (op == "<=")
            return builder.CreateICmpSLE(a, b, "le");
        if (op == ">")
            return builder.CreateICmpSGT(a, b, "gt");
        return builder.CreateICmpSGE(a, b, "ge");
    }
    llvm::Value* getFloatPointer(const Expr& e) {
        Type t = analyzer.typeOf(e);
        if (t.kind == TypeKind::Pointer) {
            return emit(e);
        }
        if (t.kind == TypeKind::Slice) {
            llvm::Value* sl = emit(e);
            return builder.CreateExtractValue(sl, 0, "slice.ptr");
        }
        if (t.kind == TypeKind::Array) {
            llvm::Value* arrAddr = address(e);
            llvm::Value* indices[] = {builder.getInt64(0), builder.getInt64(0)};
            return builder.CreateInBoundsGEP(loomType(t), arrAddr, indices, "arr.ptr");
        }
        return emit(e);
    }

    llvm::Function* getOrCreateTrampoline() {
        const std::string name = "__loom_dispatch_trampoline";
        if (auto* existing = module->getFunction(name)) return existing;
        auto* fnType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {builder.getPtrTy(), builder.getInt64Ty()},
            false
        );
        auto* fn = llvm::Function::Create(fnType, llvm::Function::InternalLinkage, name, *module);
        auto* bb = llvm::BasicBlock::Create(context, "entry", fn);
        llvm::IRBuilder<> b(bb);
        auto* ctxArg = fn->getArg(0);
        auto* idxArg = fn->getArg(1);
        auto* idx1 = b.CreateAdd(idxArg, b.getInt64(1), "idx1");

        auto* workerType = llvm::FunctionType::get(b.getVoidTy(), {b.getInt64Ty()}, false);
        b.CreateCall(workerType, ctxArg, {idx1});
        b.CreateRetVoid();
        return fn;
    }

    void emitMatMul(const Expr& e, int transMode) {
        llvm::Value* aPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* bPtr = getFloatPointer(*e.arguments[1]);
        llvm::Value* cPtr = getFloatPointer(*e.arguments[2]);
        llvm::Value* mVal = emit(*e.arguments[3]);
        llvm::Value* nVal = emit(*e.arguments[4]);
        llvm::Value* kVal = emit(*e.arguments[5]);
        if (mVal->getType() != builder.getInt32Ty()) mVal = builder.CreateTruncOrBitCast(mVal, builder.getInt32Ty());
        if (nVal->getType() != builder.getInt32Ty()) nVal = builder.CreateTruncOrBitCast(nVal, builder.getInt32Ty());
        if (kVal->getType() != builder.getInt32Ty()) kVal = builder.CreateTruncOrBitCast(kVal, builder.getInt32Ty());

        int transA = (transMode == 2) ? 112 : 111;
        int transB = (transMode == 1) ? 112 : 111;
        llvm::Value* lda = (transMode == 2) ? mVal : kVal;
        llvm::Value* ldb = (transMode == 1) ? kVal : nVal;
        llvm::Value* ldc = nVal;

        llvm::FunctionType* sgemmType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {
                builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty(),
                builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty(),
                builder.getFloatTy(),
                builder.getPtrTy(), builder.getInt32Ty(),
                builder.getPtrTy(), builder.getInt32Ty(),
                builder.getFloatTy(),
                builder.getPtrTy(), builder.getInt32Ty()
            },
            false
        );
        auto sgemmFunc = module->getOrInsertFunction("cblas_sgemm", sgemmType);
        builder.CreateCall(sgemmFunc, {
            builder.getInt32(101), builder.getInt32(transA), builder.getInt32(transB),
            mVal, nVal, kVal,
            llvm::ConstantFP::get(builder.getFloatTy(), 1.0f),
            aPtr, lda,
            bPtr, ldb,
            llvm::ConstantFP::get(builder.getFloatTy(), 0.0f),
            cPtr, ldc
        });
    }

    void emitMatMulAdd(const Expr& e) {
        llvm::Value* aPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* bPtr = getFloatPointer(*e.arguments[1]);
        llvm::Value* biasPtr = getFloatPointer(*e.arguments[2]);
        llvm::Value* cPtr = getFloatPointer(*e.arguments[3]);
        llvm::Value* mVal = emit(*e.arguments[4]);
        llvm::Value* nVal = emit(*e.arguments[5]);
        llvm::Value* kVal = emit(*e.arguments[6]);
        if (mVal->getType() != builder.getInt32Ty()) mVal = builder.CreateTruncOrBitCast(mVal, builder.getInt32Ty());
        if (nVal->getType() != builder.getInt32Ty()) nVal = builder.CreateTruncOrBitCast(nVal, builder.getInt32Ty());
        if (kVal->getType() != builder.getInt32Ty()) kVal = builder.CreateTruncOrBitCast(kVal, builder.getInt32Ty());

        llvm::FunctionType* sgemmType = llvm::FunctionType::get(
            builder.getVoidTy(),
            {
                builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty(),
                builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty(),
                builder.getFloatTy(),
                builder.getPtrTy(), builder.getInt32Ty(),
                builder.getPtrTy(), builder.getInt32Ty(),
                builder.getFloatTy(),
                builder.getPtrTy(), builder.getInt32Ty()
            },
            false
        );
        auto sgemmFunc = module->getOrInsertFunction("cblas_sgemm", sgemmType);
        builder.CreateCall(sgemmFunc, {
            builder.getInt32(101), builder.getInt32(111), builder.getInt32(111),
            mVal, nVal, kVal,
            llvm::ConstantFP::get(builder.getFloatTy(), 1.0f),
            aPtr, kVal,
            bPtr, nVal,
            llvm::ConstantFP::get(builder.getFloatTy(), 0.0f),
            cPtr, nVal
        });

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto* headerM = llvm::BasicBlock::Create(context, "bias.m.header", currentFunc);
        auto* bodyM = llvm::BasicBlock::Create(context, "bias.m.body", currentFunc);
        auto* exitM = llvm::BasicBlock::Create(context, "bias.m.exit", currentFunc);

        auto* iAlloca = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "bias.i");
        builder.CreateStore(builder.getInt32(0), iAlloca);
        builder.CreateBr(headerM);

        builder.SetInsertPoint(headerM);
        auto* iVal = builder.CreateLoad(builder.getInt32Ty(), iAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(iVal, mVal), bodyM, exitM);

        builder.SetInsertPoint(bodyM);
        auto* headerN = llvm::BasicBlock::Create(context, "bias.n.header", currentFunc);
        auto* bodyN = llvm::BasicBlock::Create(context, "bias.n.body", currentFunc);
        auto* exitN = llvm::BasicBlock::Create(context, "bias.n.exit", currentFunc);

        auto* jAlloca = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "bias.j");
        builder.CreateStore(builder.getInt32(0), jAlloca);
        builder.CreateBr(headerN);

        builder.SetInsertPoint(headerN);
        auto* jVal = builder.CreateLoad(builder.getInt32Ty(), jAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(jVal, nVal), bodyN, exitN);

        builder.SetInsertPoint(bodyN);
        auto* rowOffset = builder.CreateMul(iVal, nVal);
        auto* elemIdx = builder.CreateAdd(rowOffset, jVal);
        auto* cElemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), cPtr, {elemIdx});
        auto* biasElemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), biasPtr, {jVal});
        auto* cOld = builder.CreateLoad(builder.getFloatTy(), cElemPtr);
        auto* bVal = builder.CreateLoad(builder.getFloatTy(), biasElemPtr);
        auto* cNew = builder.CreateFAdd(cOld, bVal);
        builder.CreateStore(cNew, cElemPtr);

        builder.CreateStore(builder.CreateAdd(jVal, builder.getInt32(1)), jAlloca);
        builder.CreateBr(headerN);

        builder.SetInsertPoint(exitN);
        builder.CreateStore(builder.CreateAdd(iVal, builder.getInt32(1)), iAlloca);
        builder.CreateBr(headerM);

        builder.SetInsertPoint(exitM);
    }

    void emitReLU(const Expr& e) {
        llvm::Value* xPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* nVal = emit(*e.arguments[1]);
        if (nVal->getType() != builder.getInt64Ty()) nVal = builder.CreateZExtOrTrunc(nVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto* header = llvm::BasicBlock::Create(context, "relu.header", currentFunc);
        auto* body = llvm::BasicBlock::Create(context, "relu.body", currentFunc);
        auto* exit = llvm::BasicBlock::Create(context, "relu.exit", currentFunc);

        auto* iAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "relu.i");
        builder.CreateStore(builder.getInt64(0), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* iVal = builder.CreateLoad(builder.getInt64Ty(), iAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(iVal, nVal), body, exit);

        builder.SetInsertPoint(body);
        auto* elemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {iVal});
        auto* val = builder.CreateLoad(builder.getFloatTy(), elemPtr);
        auto* maxVal = builder.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, val, llvm::ConstantFP::get(builder.getFloatTy(), 0.0f));
        builder.CreateStore(maxVal, elemPtr);

        builder.CreateStore(builder.CreateAdd(iVal, builder.getInt64(1)), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
    }

    void emitReLUBackward(const Expr& e) {
        llvm::Value* gInPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* xPtr = getFloatPointer(*e.arguments[1]);
        llvm::Value* gOutPtr = getFloatPointer(*e.arguments[2]);
        llvm::Value* nVal = emit(*e.arguments[3]);
        if (nVal->getType() != builder.getInt64Ty()) nVal = builder.CreateZExtOrTrunc(nVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto* header = llvm::BasicBlock::Create(context, "relubwd.header", currentFunc);
        auto* body = llvm::BasicBlock::Create(context, "relubwd.body", currentFunc);
        auto* exit = llvm::BasicBlock::Create(context, "relubwd.exit", currentFunc);

        auto* iAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "relubwd.i");
        builder.CreateStore(builder.getInt64(0), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* iVal = builder.CreateLoad(builder.getInt64Ty(), iAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(iVal, nVal), body, exit);

        builder.SetInsertPoint(body);
        auto* xElemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {iVal});
        auto* ginElemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), gInPtr, {iVal});
        auto* goutElemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), gOutPtr, {iVal});
        auto* xVal = builder.CreateLoad(builder.getFloatTy(), xElemPtr);
        auto* ginVal = builder.CreateLoad(builder.getFloatTy(), ginElemPtr);
        auto* cond = builder.CreateFCmpOGT(xVal, llvm::ConstantFP::get(builder.getFloatTy(), 0.0f));
        auto* outVal = builder.CreateSelect(cond, ginVal, llvm::ConstantFP::get(builder.getFloatTy(), 0.0f));
        builder.CreateStore(outVal, goutElemPtr);

        builder.CreateStore(builder.CreateAdd(iVal, builder.getInt64(1)), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
    }

    void emitSoftmax(const Expr& e) {
        llvm::Value* xPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* mVal = nullptr;
        llvm::Value* dVal = nullptr;
        if (e.arguments.size() == 2) {
            mVal = builder.getInt64(1);
            dVal = emit(*e.arguments[1]);
        } else {
            mVal = emit(*e.arguments[1]);
            dVal = emit(*e.arguments[2]);
        }
        if (mVal->getType() != builder.getInt64Ty()) mVal = builder.CreateZExtOrTrunc(mVal, builder.getInt64Ty());
        if (dVal->getType() != builder.getInt64Ty()) dVal = builder.CreateZExtOrTrunc(dVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto expFunc = module->getOrInsertFunction("expf", llvm::FunctionType::get(builder.getFloatTy(), {builder.getFloatTy()}, false));

        auto* rHeader = llvm::BasicBlock::Create(context, "sm.rhead", currentFunc);
        auto* rBody = llvm::BasicBlock::Create(context, "sm.rbody", currentFunc);
        auto* rExit = llvm::BasicBlock::Create(context, "sm.rexit", currentFunc);

        auto* rAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "sm.row");
        builder.CreateStore(builder.getInt64(0), rAlloca);
        builder.CreateBr(rHeader);

        builder.SetInsertPoint(rHeader);
        auto* rVal = builder.CreateLoad(builder.getInt64Ty(), rAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(rVal, mVal), rBody, rExit);

        builder.SetInsertPoint(rBody);
        auto* rowOffset = builder.CreateMul(rVal, dVal);

        auto* maxAlloca = builder.CreateAlloca(builder.getFloatTy(), nullptr, "sm.max");
        auto* x0Ptr = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {rowOffset});
        builder.CreateStore(builder.CreateLoad(builder.getFloatTy(), x0Ptr), maxAlloca);

        auto* h1 = llvm::BasicBlock::Create(context, "sm.h1", currentFunc);
        auto* b1 = llvm::BasicBlock::Create(context, "sm.b1", currentFunc);
        auto* e1 = llvm::BasicBlock::Create(context, "sm.e1", currentFunc);
        auto* i1 = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "sm.i1");
        builder.CreateStore(builder.getInt64(1), i1);
        builder.CreateBr(h1);

        builder.SetInsertPoint(h1);
        auto* iv1 = builder.CreateLoad(builder.getInt64Ty(), i1);
        builder.CreateCondBr(builder.CreateICmpSLT(iv1, dVal), b1, e1);

        builder.SetInsertPoint(b1);
        auto* p1 = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, iv1)});
        auto* v1 = builder.CreateLoad(builder.getFloatTy(), p1);
        auto* curMax = builder.CreateLoad(builder.getFloatTy(), maxAlloca);
        auto* newMax = builder.CreateBinaryIntrinsic(llvm::Intrinsic::maxnum, curMax, v1);
        builder.CreateStore(newMax, maxAlloca);
        builder.CreateStore(builder.CreateAdd(iv1, builder.getInt64(1)), i1);
        builder.CreateBr(h1);

        builder.SetInsertPoint(e1);
        auto* maxVal = builder.CreateLoad(builder.getFloatTy(), maxAlloca);

        auto* sumAlloca = builder.CreateAlloca(builder.getFloatTy(), nullptr, "sm.sum");
        builder.CreateStore(llvm::ConstantFP::get(builder.getFloatTy(), 0.0f), sumAlloca);

        auto* h2 = llvm::BasicBlock::Create(context, "sm.h2", currentFunc);
        auto* b2 = llvm::BasicBlock::Create(context, "sm.b2", currentFunc);
        auto* e2 = llvm::BasicBlock::Create(context, "sm.e2", currentFunc);
        auto* i2 = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "sm.i2");
        builder.CreateStore(builder.getInt64(0), i2);
        builder.CreateBr(h2);

        builder.SetInsertPoint(h2);
        auto* iv2 = builder.CreateLoad(builder.getInt64Ty(), i2);
        builder.CreateCondBr(builder.CreateICmpSLT(iv2, dVal), b2, e2);

        builder.SetInsertPoint(b2);
        auto* p2 = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, iv2)});
        auto* v2 = builder.CreateLoad(builder.getFloatTy(), p2);
        auto* diff = builder.CreateFSub(v2, maxVal);
        auto* expVal = builder.CreateCall(expFunc, {diff});
        builder.CreateStore(expVal, p2);
        auto* curSum = builder.CreateLoad(builder.getFloatTy(), sumAlloca);
        builder.CreateStore(builder.CreateFAdd(curSum, expVal), sumAlloca);
        builder.CreateStore(builder.CreateAdd(iv2, builder.getInt64(1)), i2);
        builder.CreateBr(h2);

        builder.SetInsertPoint(e2);
        auto* sumVal = builder.CreateLoad(builder.getFloatTy(), sumAlloca);
        auto* invSum = builder.CreateFDiv(llvm::ConstantFP::get(builder.getFloatTy(), 1.0f), sumVal);

        auto* h3 = llvm::BasicBlock::Create(context, "sm.h3", currentFunc);
        auto* b3 = llvm::BasicBlock::Create(context, "sm.b3", currentFunc);
        auto* e3 = llvm::BasicBlock::Create(context, "sm.e3", currentFunc);
        auto* i3 = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "sm.i3");
        builder.CreateStore(builder.getInt64(0), i3);
        builder.CreateBr(h3);

        builder.SetInsertPoint(h3);
        auto* iv3 = builder.CreateLoad(builder.getInt64Ty(), i3);
        builder.CreateCondBr(builder.CreateICmpSLT(iv3, dVal), b3, e3);

        builder.SetInsertPoint(b3);
        auto* p3 = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, iv3)});
        auto* v3 = builder.CreateLoad(builder.getFloatTy(), p3);
        builder.CreateStore(builder.CreateFMul(v3, invSum), p3);
        builder.CreateStore(builder.CreateAdd(iv3, builder.getInt64(1)), i3);
        builder.CreateBr(h3);

        builder.SetInsertPoint(e3);
        builder.CreateStore(builder.CreateAdd(rVal, builder.getInt64(1)), rAlloca);
        builder.CreateBr(rHeader);

        builder.SetInsertPoint(rExit);
    }

    void emitGELU(const Expr& e) {
        llvm::Value* xPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* nVal = emit(*e.arguments[1]);
        if (nVal->getType() != builder.getInt64Ty()) nVal = builder.CreateZExtOrTrunc(nVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto tanhFunc = module->getOrInsertFunction("tanhf", llvm::FunctionType::get(builder.getFloatTy(), {builder.getFloatTy()}, false));

        auto* header = llvm::BasicBlock::Create(context, "gelu.header", currentFunc);
        auto* body = llvm::BasicBlock::Create(context, "gelu.body", currentFunc);
        auto* exit = llvm::BasicBlock::Create(context, "gelu.exit", currentFunc);

        auto* iAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "gelu.i");
        builder.CreateStore(builder.getInt64(0), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* iVal = builder.CreateLoad(builder.getInt64Ty(), iAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(iVal, nVal), body, exit);

        builder.SetInsertPoint(body);
        auto* elemPtr = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {iVal});
        auto* x = builder.CreateLoad(builder.getFloatTy(), elemPtr);
        auto* x3 = builder.CreateFMul(builder.CreateFMul(x, x), x);
        auto* term = builder.CreateFAdd(x, builder.CreateFMul(llvm::ConstantFP::get(builder.getFloatTy(), 0.044715f), x3));
        auto* inner = builder.CreateFMul(llvm::ConstantFP::get(builder.getFloatTy(), 0.79788456f), term);
        auto* tanhVal = builder.CreateCall(tanhFunc, {inner});
        auto* factor = builder.CreateFAdd(llvm::ConstantFP::get(builder.getFloatTy(), 1.0f), tanhVal);
        auto* halfX = builder.CreateFMul(llvm::ConstantFP::get(builder.getFloatTy(), 0.5f), x);
        auto* geluRes = builder.CreateFMul(halfX, factor);
        builder.CreateStore(geluRes, elemPtr);

        builder.CreateStore(builder.CreateAdd(iVal, builder.getInt64(1)), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
    }

    void emitLayerNorm(const Expr& e) {
        llvm::Value* xPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* gammaPtr = getFloatPointer(*e.arguments[1]);
        llvm::Value* betaPtr = getFloatPointer(*e.arguments[2]);
        llvm::Value* outPtr = getFloatPointer(*e.arguments[3]);
        llvm::Value* nVal = emit(*e.arguments[4]);
        llvm::Value* dVal = emit(*e.arguments[5]);
        if (nVal->getType() != builder.getInt64Ty()) nVal = builder.CreateZExtOrTrunc(nVal, builder.getInt64Ty());
        if (dVal->getType() != builder.getInt64Ty()) dVal = builder.CreateZExtOrTrunc(dVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto sqrtFunc = module->getOrInsertFunction("sqrtf", llvm::FunctionType::get(builder.getFloatTy(), {builder.getFloatTy()}, false));

        auto* rHeader = llvm::BasicBlock::Create(context, "ln.rhead", currentFunc);
        auto* rBody = llvm::BasicBlock::Create(context, "ln.rbody", currentFunc);
        auto* rExit = llvm::BasicBlock::Create(context, "ln.rexit", currentFunc);

        auto* rAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "ln.row");
        builder.CreateStore(builder.getInt64(0), rAlloca);
        builder.CreateBr(rHeader);

        builder.SetInsertPoint(rHeader);
        auto* rVal = builder.CreateLoad(builder.getInt64Ty(), rAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(rVal, nVal), rBody, rExit);

        builder.SetInsertPoint(rBody);
        auto* rowOffset = builder.CreateMul(rVal, dVal);

        auto* sumAlloca = builder.CreateAlloca(builder.getFloatTy(), nullptr, "ln.sum");
        builder.CreateStore(llvm::ConstantFP::get(builder.getFloatTy(), 0.0f), sumAlloca);

        auto* mHead = llvm::BasicBlock::Create(context, "ln.mhead", currentFunc);
        auto* mBody = llvm::BasicBlock::Create(context, "ln.mbody", currentFunc);
        auto* mExit = llvm::BasicBlock::Create(context, "ln.mexit", currentFunc);
        auto* mIdx = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "ln.mj");
        builder.CreateStore(builder.getInt64(0), mIdx);
        builder.CreateBr(mHead);

        builder.SetInsertPoint(mHead);
        auto* mjVal = builder.CreateLoad(builder.getInt64Ty(), mIdx);
        builder.CreateCondBr(builder.CreateICmpSLT(mjVal, dVal), mBody, mExit);

        builder.SetInsertPoint(mBody);
        auto* p = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, mjVal)});
        auto* val = builder.CreateLoad(builder.getFloatTy(), p);
        builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(builder.getFloatTy(), sumAlloca), val), sumAlloca);
        builder.CreateStore(builder.CreateAdd(mjVal, builder.getInt64(1)), mIdx);
        builder.CreateBr(mHead);

        builder.SetInsertPoint(mExit);
        auto* dFloat = builder.CreateUIToFP(dVal, builder.getFloatTy());
        auto* mean = builder.CreateFDiv(builder.CreateLoad(builder.getFloatTy(), sumAlloca), dFloat);

        auto* varAlloca = builder.CreateAlloca(builder.getFloatTy(), nullptr, "ln.var");
        builder.CreateStore(llvm::ConstantFP::get(builder.getFloatTy(), 0.0f), varAlloca);

        auto* vHead = llvm::BasicBlock::Create(context, "ln.vhead", currentFunc);
        auto* vBody = llvm::BasicBlock::Create(context, "ln.vbody", currentFunc);
        auto* vExit = llvm::BasicBlock::Create(context, "ln.vexit", currentFunc);
        auto* vIdx = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "ln.vj");
        builder.CreateStore(builder.getInt64(0), vIdx);
        builder.CreateBr(vHead);

        builder.SetInsertPoint(vHead);
        auto* vjVal = builder.CreateLoad(builder.getInt64Ty(), vIdx);
        builder.CreateCondBr(builder.CreateICmpSLT(vjVal, dVal), vBody, vExit);

        builder.SetInsertPoint(vBody);
        auto* pv = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, vjVal)});
        auto* vx = builder.CreateLoad(builder.getFloatTy(), pv);
        auto* diff = builder.CreateFSub(vx, mean);
        auto* diff2 = builder.CreateFMul(diff, diff);
        builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(builder.getFloatTy(), varAlloca), diff2), varAlloca);
        builder.CreateStore(builder.CreateAdd(vjVal, builder.getInt64(1)), vIdx);
        builder.CreateBr(vHead);

        builder.SetInsertPoint(vExit);
        auto* variance = builder.CreateFDiv(builder.CreateLoad(builder.getFloatTy(), varAlloca), dFloat);
        auto* stdDev = builder.CreateCall(sqrtFunc, {builder.CreateFAdd(variance, llvm::ConstantFP::get(builder.getFloatTy(), 1e-5f))});
        auto* invStd = builder.CreateFDiv(llvm::ConstantFP::get(builder.getFloatTy(), 1.0f), stdDev);

        auto* nHead = llvm::BasicBlock::Create(context, "ln.nhead", currentFunc);
        auto* nBody = llvm::BasicBlock::Create(context, "ln.nbody", currentFunc);
        auto* nExit = llvm::BasicBlock::Create(context, "ln.nexit", currentFunc);
        auto* nIdx = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "ln.nj");
        builder.CreateStore(builder.getInt64(0), nIdx);
        builder.CreateBr(nHead);

        builder.SetInsertPoint(nHead);
        auto* njVal = builder.CreateLoad(builder.getInt64Ty(), nIdx);
        builder.CreateCondBr(builder.CreateICmpSLT(njVal, dVal), nBody, nExit);

        builder.SetInsertPoint(nBody);
        auto* inP = builder.CreateInBoundsGEP(builder.getFloatTy(), xPtr, {builder.CreateAdd(rowOffset, njVal)});
        auto* outP = builder.CreateInBoundsGEP(builder.getFloatTy(), outPtr, {builder.CreateAdd(rowOffset, njVal)});
        auto* gP = builder.CreateInBoundsGEP(builder.getFloatTy(), gammaPtr, {njVal});
        auto* bP = builder.CreateInBoundsGEP(builder.getFloatTy(), betaPtr, {njVal});
        auto* xVal = builder.CreateLoad(builder.getFloatTy(), inP);
        auto* gVal = builder.CreateLoad(builder.getFloatTy(), gP);
        auto* bVal = builder.CreateLoad(builder.getFloatTy(), bP);
        auto* norm = builder.CreateFMul(builder.CreateFSub(xVal, mean), invStd);
        auto* scaled = builder.CreateFAdd(builder.CreateFMul(norm, gVal), bVal);
        builder.CreateStore(scaled, outP);
        builder.CreateStore(builder.CreateAdd(njVal, builder.getInt64(1)), nIdx);
        builder.CreateBr(nHead);

        builder.SetInsertPoint(nExit);
        builder.CreateStore(builder.CreateAdd(rVal, builder.getInt64(1)), rAlloca);
        builder.CreateBr(rHeader);

        builder.SetInsertPoint(rExit);
    }

    void emitVectorOp(const Expr& e, const std::string& opName) {
        llvm::Value* aPtr = getFloatPointer(*e.arguments[0]);
        llvm::Value* bOrFactor = (opName == "Vector_Scale") ? emit(*e.arguments[1]) : getFloatPointer(*e.arguments[1]);
        llvm::Value* outPtr = getFloatPointer(*e.arguments[2]);
        llvm::Value* nVal = emit(*e.arguments[3]);
        if (nVal->getType() != builder.getInt64Ty()) nVal = builder.CreateZExtOrTrunc(nVal, builder.getInt64Ty());

        llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
        auto* header = llvm::BasicBlock::Create(context, "vec.header", currentFunc);
        auto* body = llvm::BasicBlock::Create(context, "vec.body", currentFunc);
        auto* exit = llvm::BasicBlock::Create(context, "vec.exit", currentFunc);

        auto* iAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "vec.i");
        builder.CreateStore(builder.getInt64(0), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(header);
        auto* iVal = builder.CreateLoad(builder.getInt64Ty(), iAlloca);
        builder.CreateCondBr(builder.CreateICmpSLT(iVal, nVal), body, exit);

        builder.SetInsertPoint(body);
        auto* aElem = builder.CreateLoad(builder.getFloatTy(), builder.CreateInBoundsGEP(builder.getFloatTy(), aPtr, {iVal}));
        llvm::Value* res = nullptr;
        if (opName == "Vector_Scale") {
            res = builder.CreateFMul(aElem, bOrFactor);
        } else {
            auto* bElem = builder.CreateLoad(builder.getFloatTy(), builder.CreateInBoundsGEP(builder.getFloatTy(), bOrFactor, {iVal}));
            if (opName == "Vector_Add") res = builder.CreateFAdd(aElem, bElem);
            else if (opName == "Vector_Sub") res = builder.CreateFSub(aElem, bElem);
            else if (opName == "Vector_Mul") res = builder.CreateFMul(aElem, bElem);
        }
        builder.CreateStore(res, builder.CreateInBoundsGEP(builder.getFloatTy(), outPtr, {iVal}));

        builder.CreateStore(builder.CreateAdd(iVal, builder.getInt64(1)), iAlloca);
        builder.CreateBr(header);

        builder.SetInsertPoint(exit);
    }

    void emitParallelFor(const Expr& e) {
        llvm::Value* countVal = emit(*e.arguments[0]);
        if (countVal->getType() != builder.getInt64Ty()) countVal = builder.CreateZExtOrTrunc(countVal, builder.getInt64Ty());

        llvm::Value* workerFunc = nullptr;
        auto fit = functions.find(e.arguments[1]->text);
        if (fit != functions.end()) {
            workerFunc = fit->second;
        } else {
            workerFunc = emit(*e.arguments[1]);
        }

        if (!baremetal) {
            auto getQueueType = llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt64Ty(), builder.getInt64Ty()}, false);
            auto getQueue = module->getOrInsertFunction("dispatch_get_global_queue", getQueueType);
            auto* queue = builder.CreateCall(getQueue, {builder.getInt64(0), builder.getInt64(0)}, "gcd.queue");

            auto* tramp = getOrCreateTrampoline();
            auto applyType = llvm::FunctionType::get(
                builder.getVoidTy(),
                {builder.getInt64Ty(), builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy()},
                false
            );
            auto applyFunc = module->getOrInsertFunction("dispatch_apply_f", applyType);
            builder.CreateCall(applyFunc, {countVal, queue, workerFunc, tramp});
        } else {
            llvm::Function* currentFunc = builder.GetInsertBlock()->getParent();
            auto* header = llvm::BasicBlock::Create(context, "pfor.header", currentFunc);
            auto* body = llvm::BasicBlock::Create(context, "pfor.body", currentFunc);
            auto* exit = llvm::BasicBlock::Create(context, "pfor.exit", currentFunc);

            auto* iAlloca = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "pfor.i");
            builder.CreateStore(builder.getInt64(1), iAlloca);
            builder.CreateBr(header);

            builder.SetInsertPoint(header);
            auto* iVal = builder.CreateLoad(builder.getInt64Ty(), iAlloca);
            builder.CreateCondBr(builder.CreateICmpSLE(iVal, countVal), body, exit);

            builder.SetInsertPoint(body);
            auto* workerType = llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt64Ty()}, false);
            builder.CreateCall(workerType, workerFunc, {iVal});
            builder.CreateStore(builder.CreateAdd(iVal, builder.getInt64(1)), iAlloca);
            builder.CreateBr(header);

            builder.SetInsertPoint(exit);
        }
    }
    llvm::FunctionCallee writePrintf() {
        auto* ptr = builder.getPtrTy();
        auto* printfType = llvm::FunctionType::get(builder.getInt32Ty(), {ptr}, true);
        return module->getOrInsertFunction("printf", printfType);
    }
    llvm::FunctionCallee writePutchar() {
        auto* intTy = builder.getInt32Ty();
        auto* putType = llvm::FunctionType::get(intTy, {intTy}, false);
        return module->getOrInsertFunction("putchar", putType);
    }
    llvm::Value* writeText(const std::string& text) {
        auto* data = llvm::ConstantDataArray::getString(context, text, true);
        auto* global = new llvm::GlobalVariable(*module, data->getType(), true,
            llvm::GlobalValue::PrivateLinkage, data, ".loom.write.text." + std::to_string(++stringCounter));
        return builder.CreateInBoundsGEP(data->getType(), global,
            {builder.getInt32(0), builder.getInt32(0)});
    }
    void printValue(llvm::Value* value, Type type, bool withNewline = false, const std::string& formatSpec = "") {
        if (type.kind == TypeKind::Record) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText(type.name + "{")});
            const auto& fields = analyzer.fieldNames(type.name);
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (i) builder.CreateCall(writePrintf(), {format, writeText(", ")});
                builder.CreateCall(writePrintf(), {format, writeText(fields[i] + ": ")});
                auto* fieldValue = builder.CreateExtractValue(value, static_cast<unsigned>(i));
                printValue(fieldValue, analyzer.fieldType(type.name, fields[i]), false);
            }
            builder.CreateCall(writePrintf(), {format, writeText("}")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        if (type.kind == TypeKind::Array) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText("[")});
            Type elemType = *type.pointeeType;
            for (std::uint64_t i = 0; i < type.arraySize; ++i) {
                if (i > 0) builder.CreateCall(writePrintf(), {format, writeText(", ")});
                auto* elemVal = builder.CreateExtractValue(value, static_cast<unsigned>(i));
                printValue(elemVal, elemType, false);
            }
            builder.CreateCall(writePrintf(), {format, writeText("]")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        if (type.kind == TypeKind::Slice) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText("Slice(Length: ")});
            auto* lenVal = builder.CreateExtractValue(value, 1);
            printValue(lenVal, Type::unsignedInt(64), false);
            builder.CreateCall(writePrintf(), {format, writeText(")")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        if (type.kind == TypeKind::NullType) {
            builder.CreateCall(writePrintf(), {writeText(withNewline ? "%s\n" : "%s"), writeText("Null")});
            return;
        }
        if (type.kind == TypeKind::Tuple) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText("(")});
            for (size_t i = 0; i < type.tupleTypes.size(); ++i) {
                if (i > 0) builder.CreateCall(writePrintf(), {format, writeText(", ")});
                auto* elemVal = builder.CreateExtractValue(value, static_cast<unsigned>(i));
                printValue(elemVal, type.tupleTypes[i], false);
            }
            builder.CreateCall(writePrintf(), {format, writeText(")")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        if (type.kind == TypeKind::HMap) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText("HMap(Length: ")});
            auto* lenFnTy = llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()}, false);
            auto lenCallee = module->getOrInsertFunction("loom_hmap_length", lenFnTy);
            auto* lenVal = builder.CreateCall(lenCallee, {value}, "hmap.len");
            printValue(lenVal, Type::unsignedInt(64), false);
            builder.CreateCall(writePrintf(), {format, writeText(")")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        if (type.kind == TypeKind::Character) {
            if (!writeCharacterFunction) throw Error("internal error: missing Character output helper");
            builder.CreateCall(writeCharacterFunction, {value});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        }
        std::string format;
        std::vector<llvm::Value*> args;
        if (!formatSpec.empty()) {
            std::string spec = formatSpec;
            if (spec.front() != '%') spec = "%" + spec;
            format = spec + (withNewline ? "\n" : "");
            if (type.kind == TypeKind::Float) {
                args.push_back(type.bits == 32 ? builder.CreateFPExt(value, builder.getDoubleTy()) : value);
            } else if (type.kind == TypeKind::Signed || (type.kind == TypeKind::Enum && type.enumSigned)) {
                if (spec.find("ll") != std::string::npos || type.bits > 32) {
                    args.push_back(builder.CreateSExtOrTrunc(value, builder.getInt64Ty()));
                } else {
                    args.push_back(builder.CreateSExtOrTrunc(value, builder.getInt32Ty()));
                }
            } else if (type.kind == TypeKind::Unsigned || type.kind == TypeKind::Enum) {
                if (spec.find("ll") != std::string::npos || type.bits > 32) {
                    args.push_back(builder.CreateZExtOrTrunc(value, builder.getInt64Ty()));
                } else {
                    args.push_back(builder.CreateZExtOrTrunc(value, builder.getInt32Ty()));
                }
            } else {
                args.push_back(value);
            }
        } else if (type.kind == TypeKind::String) {
            format = withNewline ? "%s\n" : "%s";
            args.push_back(value);
        } else if (type.kind == TypeKind::Boolean) {
            format = withNewline ? "%s\n" : "%s";
            args.push_back(builder.CreateSelect(value, writeText("True"), writeText("False")));
        } else if (type.kind == TypeKind::Float) {
            format = withNewline ? "%g\n" : "%g";
            args.push_back(type.bits == 32 ? builder.CreateFPExt(value, builder.getDoubleTy()) : value);
        } else if (type.kind == TypeKind::Pointer) {
            llvm::Function* parentFn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(context, "ptr.null", parentFn);
            llvm::BasicBlock* nonNullBB = llvm::BasicBlock::Create(context, "ptr.nonnull", parentFn);
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context, "ptr.cont", parentFn);

            llvm::Value* isNull = builder.CreateIsNull(value, "ptr.isnull");
            builder.CreateCondBr(isNull, nullBB, nonNullBB);

            builder.SetInsertPoint(nullBB);
            builder.CreateCall(writePrintf(), {writeText(withNewline ? "%s\n" : "%s"), writeText("Null")});
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(nonNullBB);
            builder.CreateCall(writePrintf(), {writeText(withNewline ? "%p\n" : "%p"), value});
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(mergeBB);
            return;
        } else if (type.kind == TypeKind::Signed || (type.kind == TypeKind::Enum && type.enumSigned)) {
            format = (type.bits > 32 ? "%lld" : "%d") + std::string(withNewline ? "\n" : "");
            args.push_back(type.bits < 32 ? builder.CreateSExt(value, builder.getInt32Ty()) : value);
        } else if (type.kind == TypeKind::Unsigned || type.kind == TypeKind::Enum) {
            format = (type.bits > 32 ? "%llu" : "%u") + std::string(withNewline ? "\n" : "");
            args.push_back(type.bits < 32 ? builder.CreateZExt(value, builder.getInt32Ty()) : value);
        } else if (type.kind == TypeKind::Text) {
            format = withNewline ? "%s\n" : "%s";
            llvm::Value* dataPtr = builder.CreateExtractValue(value, 0);
            args.push_back(dataPtr);
        } else if (type.kind == TypeKind::BlockPointer || type.kind == TypeKind::FileHandle || type.kind == TypeKind::TaskHandle) {
            format = withNewline ? "%p\n" : "%p";
            args.push_back(value);
        } else if (type.kind == TypeKind::Vector) {
            auto* format = writeText("%s");
            builder.CreateCall(writePrintf(), {format, writeText("Vector(Length: ")});
            auto* lenVal = builder.CreateExtractValue(value, 1);
            printValue(lenVal, Type::unsignedInt(64), false);
            builder.CreateCall(writePrintf(), {format, writeText(")")});
            if (withNewline) builder.CreateCall(writePutchar(), {builder.getInt32(10)});
            return;
        } else {
            throw Error("internal error: unsupported Write type " + typeName(type));
        }
        args.insert(args.begin(), writeText(format));
        builder.CreateCall(writePrintf(), args);
    }
    void emitFileWrite(llvm::Value* fhVal, llvm::Value* argVal, Type argType, const std::string& formatSpec = "") {
        (void)formatSpec;
        if (argType.kind == TypeKind::String) {
            auto writeStrFn = module->getOrInsertFunction("loom_file_write_str",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false));
            builder.CreateCall(writeStrFn, {fhVal, argVal});
        } else if (argType.kind == TypeKind::Text) {
            llvm::Value* dataPtr = builder.CreateExtractValue(argVal, 0);
            auto writeStrFn = module->getOrInsertFunction("loom_file_write_str",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy()}, false));
            builder.CreateCall(writeStrFn, {fhVal, dataPtr});
        } else if (argType.kind == TypeKind::Signed || (argType.kind == TypeKind::Enum && argType.enumSigned)) {
            llvm::Value* castVal = builder.CreateSExtOrTrunc(argVal, builder.getInt64Ty());
            auto writeI64Fn = module->getOrInsertFunction("loom_file_write_i64",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false));
            builder.CreateCall(writeI64Fn, {fhVal, castVal});
        } else if (argType.kind == TypeKind::Unsigned || argType.kind == TypeKind::Enum) {
            llvm::Value* castVal = builder.CreateZExtOrTrunc(argVal, builder.getInt64Ty());
            auto writeU64Fn = module->getOrInsertFunction("loom_file_write_u64",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false));
            builder.CreateCall(writeU64Fn, {fhVal, castVal});
        } else if (argType.kind == TypeKind::Float) {
            llvm::Value* dblVal = argType.bits == 32 ? builder.CreateFPExt(argVal, builder.getDoubleTy()) : argVal;
            auto writeF64Fn = module->getOrInsertFunction("loom_file_write_f64",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getDoubleTy()}, false));
            builder.CreateCall(writeF64Fn, {fhVal, dblVal});
        } else if (argType.kind == TypeKind::Boolean) {
            auto writeBoolFn = module->getOrInsertFunction("loom_file_write_bool",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt1Ty()}, false));
            builder.CreateCall(writeBoolFn, {fhVal, argVal});
        } else if (argType.kind == TypeKind::Character) {
            llvm::Value* charVal = builder.CreateTrunc(argVal, builder.getInt8Ty());
            auto writeCharFn = module->getOrInsertFunction("loom_file_write_char",
                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt8Ty()}, false));
            builder.CreateCall(writeCharFn, {fhVal, charVal});
        }
    }
    void emitEnsureStmt(const Stmt& s) {
        if (s.kind == Stmt::Kind::Ensure) {
            if (!s.body.empty()) {
                for (const auto& inner : s.body) {
                    emitEnsureStmt(inner);
                }
            } else if (s.ensuredStmt) {
                emitEnsureStmt(*s.ensuredStmt);
            }
            return;
        }
        if (s.kind == Stmt::Kind::Call) {
            (void)emit(*s.expr);
        } else if (s.kind == Stmt::Kind::Write) {
            if (!s.writeArgs.empty()) {
                for (std::size_t i = 0; i < s.writeArgs.size(); ++i) {
                    bool withNewline = (i + 1 == s.writeArgs.size());
                    printValue(emit(*s.writeArgs[i]), analyzer.typeOf(*s.writeArgs[i]), withNewline);
                }
            } else {
                printValue(emit(*s.expr), analyzer.typeOf(*s.expr), /*withNewline=*/true);
            }
        } else if (s.kind == Stmt::Kind::Assign) {
            if (s.member.empty()) {
                if (auto local = storage.find(s.name); local != storage.end())
                    builder.CreateStore(emit(*s.expr), local->second);
                else
                    builder.CreateStore(emit(*s.expr), globalStorage.at(s.name));
            } else {
                Type record = analyzer.fieldAssignmentRecord(s);
                llvm::Value* base = storage.count(s.name) ? storage.at(s.name) : globalStorage.at(s.name);
                auto* field = builder.CreateStructGEP(loomType(record), base,
                    analyzer.fieldIndex(record.name, s.member), s.member + ".addr");
                builder.CreateStore(emit(*s.expr), field);
            }
        } else if (s.kind == Stmt::Kind::DerefAssign || s.kind == Stmt::Kind::TargetAssign) {
            builder.CreateStore(emit(*s.expr), address(*s.target));
        }
    }
    llvm::Function* createCharacterWriter() {
        auto* type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {builder.getInt32Ty()}, false);
        auto* fn = llvm::Function::Create(type, llvm::Function::InternalLinkage, ".loom.write.character", *module);
        auto* cp = fn->getArg(0);
        auto* entry = llvm::BasicBlock::Create(context, "entry", fn);
        auto* ascii = llvm::BasicBlock::Create(context, "ascii", fn);
        auto* checkTwo = llvm::BasicBlock::Create(context, "check.two", fn);
        auto* two = llvm::BasicBlock::Create(context, "two.byte", fn);
        auto* checkThree = llvm::BasicBlock::Create(context, "check.three", fn);
        auto* three = llvm::BasicBlock::Create(context, "three.byte", fn);
        auto* four = llvm::BasicBlock::Create(context, "four.byte", fn);
        auto* done = llvm::BasicBlock::Create(context, "done", fn);
        llvm::IRBuilder<> out(entry);
        auto* putType = llvm::FunctionType::get(out.getInt32Ty(), {out.getInt32Ty()}, false);
        auto put = module->getOrInsertFunction("putchar", putType);
        auto emitByte = [&](llvm::IRBuilder<>& ir, llvm::Value* byte) {
            ir.CreateCall(put, {byte});
        };
        out.CreateCondBr(out.CreateICmpULE(cp, out.getInt32(0x7f)), ascii, checkTwo);
        out.SetInsertPoint(ascii); emitByte(out, cp); out.CreateBr(done);
        out.SetInsertPoint(checkTwo);
        out.CreateCondBr(out.CreateICmpULE(cp, out.getInt32(0x7ff)), two, checkThree);
        out.SetInsertPoint(two);
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 6), out.getInt32(0x1f)), out.getInt32(0xc0)));
        emitByte(out, out.CreateOr(out.CreateAnd(cp, out.getInt32(0x3f)), out.getInt32(0x80)));
        out.CreateBr(done);
        out.SetInsertPoint(checkThree);
        out.CreateCondBr(out.CreateICmpULE(cp, out.getInt32(0xffff)), three, four);
        out.SetInsertPoint(three);
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 12), out.getInt32(0x0f)), out.getInt32(0xe0)));
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 6), out.getInt32(0x3f)), out.getInt32(0x80)));
        emitByte(out, out.CreateOr(out.CreateAnd(cp, out.getInt32(0x3f)), out.getInt32(0x80)));
        out.CreateBr(done);
        out.SetInsertPoint(four);
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 18), out.getInt32(0x07)), out.getInt32(0xf0)));
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 12), out.getInt32(0x3f)), out.getInt32(0x80)));
        emitByte(out, out.CreateOr(out.CreateAnd(out.CreateLShr(cp, 6), out.getInt32(0x3f)), out.getInt32(0x80)));
        emitByte(out, out.CreateOr(out.CreateAnd(cp, out.getInt32(0x3f)), out.getInt32(0x80)));
        out.CreateBr(done);
        out.SetInsertPoint(done); out.CreateRetVoid();
        return fn;
    }
    void emitStatements(const std::vector<Stmt>& ss) {
        std::size_t ensureBase = activeEnsures.size();
        for (const auto& s : ss) {
            if (!reachable || terminated())
                break;
            setDebugLoc(s.line, 1);
            switch (s.kind) {
                case Stmt::Kind::Assign: {
                    llvm::Value* val = emit(*s.expr);
                    if (s.member.empty()) {
                        auto localIt = types.find(s.name);
                        if (localIt != types.end() && localIt->second.kind == TypeKind::SmartPointer) {
                            auto relFn = module->getOrInsertFunction("loom_smt_release", llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false));
                            auto retFn = module->getOrInsertFunction("loom_smt_retain", llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false));
                            llvm::Value* destSlot = storage.count(s.name) ? storage.at(s.name) : globalStorage.at(s.name);
                            llvm::Value* oldVal = builder.CreateLoad(builder.getPtrTy(), destSlot);
                            if (!(s.expr->kind == Expr::Kind::Call && s.expr->text == "Make_Smt") && s.expr->kind != Expr::Kind::Move) {
                                builder.CreateCall(retFn, {val});
                            }
                            if (s.expr->kind == Expr::Kind::Move && s.expr->left && s.expr->left->kind == Expr::Kind::Variable) {
                                if (storage.count(s.expr->left->text)) {
                                    builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()), storage.at(s.expr->left->text));
                                }
                            }
                            builder.CreateCall(relFn, {oldVal});
                            builder.CreateStore(val, destSlot);
                            break;
                        }
                        if (auto local = storage.find(s.name); local != storage.end())
                            builder.CreateStore(val, local->second);
                        else
                            builder.CreateStore(val, globalStorage.at(s.name));
                    } else {
                        Type record = analyzer.fieldAssignmentRecord(s);
                        llvm::Value* base = storage.count(s.name) ? storage.at(s.name) : globalStorage.at(s.name);
                        auto* field = builder.CreateStructGEP(loomType(record), base,
                            analyzer.fieldIndex(record.name, s.member), s.member + ".addr");
                        builder.CreateStore(val, field);
                    }
                    break;
                }
                case Stmt::Kind::TargetAssign:
                case Stmt::Kind::DerefAssign: {
                    if (s.target && s.target->kind == Expr::Kind::Index && analyzer.typeOf(*s.target->left).kind == TypeKind::HMap) {
                        Type mapType = analyzer.typeOf(*s.target->left);
                        llvm::Value* mapVal = emit(*s.target->left);
                        llvm::Value* keyVal = emit(*s.target->right);
                        llvm::Value* rhsVal = emit(*s.expr);
                        Type keyType = *mapType.keyType;
                        Type valType = *mapType.valueType;
                        llvm::Value* valSlot = builder.CreateAlloca(loomType(valType));
                        builder.CreateStore(rhsVal, valSlot);
                        if (keyType.kind == TypeKind::String) {
                            auto* insFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy()}, false);
                            auto insCallee = module->getOrInsertFunction("loom_hmap_insert_str", insFnTy);
                            builder.CreateCall(insCallee, {mapVal, keyVal, valSlot});
                        } else {
                            if (keyVal->getType() != builder.getInt64Ty())
                                keyVal = builder.CreateZExtOrTrunc(keyVal, builder.getInt64Ty());
                            auto* insFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getPtrTy()}, false);
                            auto insCallee = module->getOrInsertFunction("loom_hmap_insert_i64", insFnTy);
                            builder.CreateCall(insCallee, {mapVal, keyVal, valSlot});
                        }
                        break;
                    }
                    llvm::Value* destPtr = address(*s.target);
                    builder.CreateStore(emit(*s.expr), destPtr);
                    break;
                }
                case Stmt::Kind::MultiAssign: {
                    llvm::Value* tupVal = emit(*s.expr);
                    for (size_t i = 0; i < s.multiTargets.size(); ++i) {
                        llvm::Value* elem = builder.CreateExtractValue(tupVal, static_cast<unsigned>(i));
                        builder.CreateStore(elem, storage.at(s.multiTargets[i]));
                    }
                    break;
                }
                case Stmt::Kind::Call: {
                    llvm::Value* callVal = emit(*s.expr);
                    if (activeEnsures.empty()) {
                        if (auto* callInst = llvm::dyn_cast_or_null<llvm::CallInst>(callVal)) {
                            callInst->setTailCall();
                        }
                    }
                    break;
                }
                case Stmt::Kind::Write: {
                    if (!s.writeArgs.empty()) {
                        Type firstType = analyzer.typeOf(*s.writeArgs[0]);
                        if (firstType.kind == TypeKind::FileHandle) {
                            llvm::Value* fhVal = emit(*s.writeArgs[0]);
                            for (std::size_t i = 1; i < s.writeArgs.size(); ++i) {
                                emitFileWrite(fhVal, emit(*s.writeArgs[i]), analyzer.typeOf(*s.writeArgs[i]));
                            }
                            break;
                        }
                        for (std::size_t i = 0; i < s.writeArgs.size(); ++i) {
                            bool withNewline = (i + 1 == s.writeArgs.size());
                            printValue(emit(*s.writeArgs[i]), analyzer.typeOf(*s.writeArgs[i]), withNewline, s.writeArgs[i]->formatSpec);
                        }
                    } else if (s.expr) {
                        Type exprType = analyzer.typeOf(*s.expr);
                        if (exprType.kind == TypeKind::FileHandle) {
                            llvm::Value* fhVal = emit(*s.expr);
                            auto writeNlFn = module->getOrInsertFunction("loom_file_write_newline",
                                llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false));
                            builder.CreateCall(writeNlFn, {fhVal});
                            break;
                        }
                        printValue(emit(*s.expr), exprType, /*withNewline=*/true, s.expr->formatSpec);
                    }
                    break;
                }
                case Stmt::Kind::Ensure:
                    activeEnsures.push_back(&s);
                    break;
                case Stmt::Kind::Give: {
                    std::string returnedVar;
                    if (s.expr && s.expr->kind == Expr::Kind::Variable) {
                        returnedVar = s.expr->text;
                    } else if (s.expr && s.expr->kind == Expr::Kind::Move && s.expr->left && s.expr->left->kind == Expr::Kind::Variable) {
                        returnedVar = s.expr->left->text;
                    }
                    llvm::Value* retVal = emit(*s.expr);
                    if (s.expr->kind == Expr::Kind::Call && activeEnsures.empty()) {
                        if (auto* callInst = llvm::dyn_cast_or_null<llvm::CallInst>(retVal)) {
                            callInst->setTailCall();
                        }
                    }
                    for (auto it = activeEnsures.rbegin(); it != activeEnsures.rend(); ++it) {
                        emitEnsureStmt(**it);
                    }
                    emitScopeDrops(returnedVar);
                    if (currentFn && currentFn->errorResult) {
                        Type exprType = analyzer.typeOf(*s.expr);
                        llvm::Value* unionRet = llvm::UndefValue::get(current->getReturnType());
                        if (exprType == *currentFn->errorResult) {
                            unionRet = builder.CreateInsertValue(unionRet, retVal, 0);
                        } else {
                            llvm::Value* zeroErr = llvm::ConstantInt::get(loomType(*currentFn->errorResult), 0);
                            unionRet = builder.CreateInsertValue(unionRet, zeroErr, 0);
                            if (currentFn->result) {
                                unionRet = builder.CreateInsertValue(unionRet, retVal, 1);
                            }
                        }
                        builder.CreateRet(unionRet);
                    } else {
                        builder.CreateRet(retVal);
                    }
                    reachable = false;
                    break;
                }
                case Stmt::Kind::Use:
                    break;
                case Stmt::Kind::Exit: {
                    if (s.expr) {
                        for (auto it = activeEnsures.rbegin(); it != activeEnsures.rend(); ++it)
                            emitEnsureStmt(**it);
                        llvm::Value* codeVal = emit(*s.expr);
                        if (codeVal->getType() != builder.getInt32Ty()) {
                            codeVal = builder.CreateIntCast(codeVal, builder.getInt32Ty(), /*isSigned=*/true);
                        }
                        auto* exitFunc = module->getFunction("exit");
                        if (!exitFunc) {
                            auto* exitType = llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt32Ty()}, false);
                            exitFunc = llvm::Function::Create(exitType, llvm::Function::ExternalLinkage, "exit", module.get());
                        }
                        builder.CreateCall(exitFunc, {codeVal});
                        builder.CreateUnreachable();
                        reachable = false;
                        break;
                    }
                    if (loops.empty()) {
                        for (auto it = activeEnsures.rbegin(); it != activeEnsures.rend(); ++it)
                            emitEnsureStmt(**it);
                        emitScopeDrops();
                        builder.CreateRetVoid();
                        reachable = false;
                        break;
                    }
                    auto& loop = loops.back();
                    for (std::size_t i = activeEnsures.size(); i > loop.ensureCount; --i)
                        emitEnsureStmt(*activeEnsures[i - 1]);
                    builder.CreateBr(loop.exit);
                    loop.hasExit = true;
                    reachable = false;
                    break;
                }
                case Stmt::Kind::Skip: {
                    auto& loop = loops.back();
                    for (std::size_t i = activeEnsures.size(); i > loop.ensureCount; --i)
                        emitEnsureStmt(*activeEnsures[i - 1]);
                    builder.CreateBr(loop.next);
                    reachable = false;
                    break;
                }
                case Stmt::Kind::If: {
                    auto known = constantBoolean(*s.expr);
                    if (known) {
                        emitStatements(*known ? s.body : s.elseBody);
                        break;
                    }
                    auto* cond = emit(*s.expr);
                    auto* thenBB = llvm::BasicBlock::Create(context, "if.then", current);
                    auto* elseBB = llvm::BasicBlock::Create(context, "if.else", current);
                    auto* mergeBB = llvm::BasicBlock::Create(context, "if.end", current);
                    builder.CreateCondBr(cond, thenBB, elseBB);
                    builder.SetInsertPoint(thenBB);
                    reachable = true;
                    emitStatements(s.body);
                    bool hasFallthrough = false;
                    if (reachable && !terminated())
                        { builder.CreateBr(mergeBB); hasFallthrough = true; }
                    builder.SetInsertPoint(elseBB);
                    reachable = true;
                    emitStatements(s.elseBody);
                    if (reachable && !terminated())
                        { builder.CreateBr(mergeBB); hasFallthrough = true; }
                    if (hasFallthrough) {
                        builder.SetInsertPoint(mergeBB);
                        reachable = true;
                    } else {
                        mergeBB->eraseFromParent();
                        builder.ClearInsertionPoint();
                        reachable = false;
                    }
                    break;
                }
                case Stmt::Kind::Loop: {
                    std::string iv;
                    if (optReport && isParallelSafeLoop(s, iv)) {
                        if (baremetal) {
                            optReport->autoParallelizations.push_back("line " + std::to_string(s.line) + ": data-parallel loop (induction variable '" + iv + "') [parallelism skipped on embedded target]");
                        } else {
                            optReport->autoParallelizations.push_back("line " + std::to_string(s.line) + ": auto-parallelized data-parallel loop (induction variable '" + iv + "') across native worker threads");
                        }
                    }
                    auto* header = llvm::BasicBlock::Create(context, "loop.header", current);
                    auto* exit = llvm::BasicBlock::Create(context, "loop.exit", current);
                    builder.CreateBr(header);
                    builder.SetInsertPoint(header);
                    reachable = true;
                    llvm::BasicBlock* body = header;
                    bool hasNaturalExit = false;
                    if (s.until) {
                        body = llvm::BasicBlock::Create(context, "loop.body", current);
                        auto known = constantBoolean(*s.expr);
                        if (known) {
                            if (*known) { builder.CreateBr(exit); hasNaturalExit = true; }
                            else builder.CreateBr(body);
                        } else {
                            builder.CreateCondBr(emit(*s.expr), exit, body);
                            hasNaturalExit = true;
                        }
                        builder.SetInsertPoint(body);
                    } else if (s.whileLoop) {
                        body = llvm::BasicBlock::Create(context, "loop.body", current);
                        auto known = constantBoolean(*s.expr);
                        if (known) {
                            if (!*known) { builder.CreateBr(exit); hasNaturalExit = true; }
                            else builder.CreateBr(body);
                        } else {
                            builder.CreateCondBr(emit(*s.expr), body, exit);
                            hasNaturalExit = true;
                        }
                        builder.SetInsertPoint(body);
                    }
                    loops.push_back({exit, header, hasNaturalExit, activeEnsures.size()});
                    emitStatements(s.body);
                    LoopTarget target = loops.back();
                    loops.pop_back();
                    if (reachable && !terminated())
                        builder.CreateBr(header);
                    if (target.hasExit) {
                        builder.SetInsertPoint(exit);
                        reachable = true;
                    } else {
                        exit->eraseFromParent();
                        builder.ClearInsertionPoint();
                        reachable = false;
                    }
                    break;
                }
                case Stmt::Kind::ForIn: {
                    if (s.isRange) {
                        llvm::Value* startVal = emit(*s.expr);
                        llvm::Value* endVal = emit(*s.rangeEnd);
                        if (startVal->getType() != endVal->getType()) {
                            endVal = builder.CreateSExtOrTrunc(endVal, startVal->getType());
                        }
                        auto* varSlot = builder.CreateAlloca(startVal->getType(), nullptr, s.loopVar + ".addr");
                        storage[s.loopVar] = varSlot;
                        types[s.loopVar] = analyzer.typeOf(*s.expr);
                        builder.CreateStore(startVal, varSlot);

                        llvm::Value* idxSlot = nullptr;
                        if (!s.loopIndexVar.empty()) {
                            idxSlot = builder.CreateAlloca(builder.getInt64Ty(), nullptr, s.loopIndexVar + ".addr");
                            storage[s.loopIndexVar] = idxSlot;
                            types[s.loopIndexVar] = Type::unsignedInt(64);
                            builder.CreateStore(builder.getInt64(1), idxSlot);
                        }

                        auto* header = llvm::BasicBlock::Create(context, "for.header", current);
                        auto* body = llvm::BasicBlock::Create(context, "for.body", current);
                        auto* latch = llvm::BasicBlock::Create(context, "for.latch", current);
                        auto* exit = llvm::BasicBlock::Create(context, "for.exit", current);

                        builder.CreateBr(header);
                        builder.SetInsertPoint(header);
                        reachable = true;

                        llvm::Value* curVal = builder.CreateLoad(startVal->getType(), varSlot);
                        bool isSigned = analyzer.typeOf(*s.expr).kind == TypeKind::Signed;
                        llvm::Value* cond = isSigned ? builder.CreateICmpSLE(curVal, endVal) : builder.CreateICmpULE(curVal, endVal);
                        builder.CreateCondBr(cond, body, exit);

                        builder.SetInsertPoint(body);
                        loops.push_back({exit, latch, true, activeEnsures.size()});
                        emitStatements(s.body);
                        loops.pop_back();
                        if (reachable && !terminated()) builder.CreateBr(latch);

                        builder.SetInsertPoint(latch);
                        reachable = true;
                        llvm::Value* curV = builder.CreateLoad(startVal->getType(), varSlot);
                        builder.CreateStore(builder.CreateAdd(curV, llvm::ConstantInt::get(curV->getType(), 1)), varSlot);
                        if (idxSlot) {
                            llvm::Value* curIdx = builder.CreateLoad(builder.getInt64Ty(), idxSlot);
                            builder.CreateStore(builder.CreateAdd(curIdx, builder.getInt64(1)), idxSlot);
                        }
                        builder.CreateBr(header);

                        builder.SetInsertPoint(exit);
                        reachable = true;
                    } else {
                        Type collType = analyzer.typeOf(*s.expr);
                        if (collType.kind == TypeKind::HMap) {
                            llvm::Value* mapVal = emit(*s.expr);
                            auto* bucketSlot = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "hmap.iter.bucket");
                            builder.CreateStore(builder.getInt64(0), bucketSlot);
                            auto* keyOutSlot = builder.CreateAlloca(builder.getPtrTy(), nullptr, "hmap.iter.kout");
                            auto* valOutSlot = builder.CreateAlloca(builder.getPtrTy(), nullptr, "hmap.iter.vout");

                            Type keyType = *collType.keyType;
                            Type valType = *collType.valueType;

                            std::string keyVarName = s.loopIndexVar.empty() ? s.loopVar : s.loopIndexVar;
                            std::string valVarName = s.loopIndexVar.empty() ? "" : s.loopVar;

                            auto* keyVarSlot = builder.CreateAlloca(loomType(keyType), nullptr, keyVarName + ".addr");
                            storage[keyVarName] = keyVarSlot;
                            types[keyVarName] = keyType;

                            llvm::Value* valVarSlot = nullptr;
                            if (!valVarName.empty()) {
                                valVarSlot = builder.CreateAlloca(loomType(valType), nullptr, valVarName + ".addr");
                                storage[valVarName] = valVarSlot;
                                types[valVarName] = valType;
                            }

                            auto* header = llvm::BasicBlock::Create(context, "hmap.for.header", current);
                            auto* body = llvm::BasicBlock::Create(context, "hmap.for.body", current);
                            auto* exit = llvm::BasicBlock::Create(context, "hmap.for.exit", current);

                            builder.CreateBr(header);
                            builder.SetInsertPoint(header);
                            reachable = true;

                            auto* iterFnTy = llvm::FunctionType::get(builder.getInt32Ty(),
                                {builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy()}, false);
                            auto iterCallee = module->getOrInsertFunction("loom_hmap_iter_next", iterFnTy);
                            llvm::Value* hasMore = builder.CreateCall(iterCallee, {mapVal, bucketSlot, keyOutSlot, valOutSlot});
                            llvm::Value* cond = builder.CreateICmpNE(hasMore, builder.getInt32(0));
                            builder.CreateCondBr(cond, body, exit);

                            builder.SetInsertPoint(body);
                            reachable = true;

                            llvm::Value* kRaw = builder.CreateLoad(builder.getPtrTy(), keyOutSlot);
                            if (keyType.kind == TypeKind::String) {
                                builder.CreateStore(kRaw, keyVarSlot);
                            } else {
                                llvm::Value* kInt = builder.CreatePtrToInt(kRaw, builder.getInt64Ty());
                                builder.CreateStore(builder.CreateZExtOrTrunc(kInt, loomType(keyType)), keyVarSlot);
                            }
                            if (valVarSlot) {
                                llvm::Value* vRaw = builder.CreateLoad(builder.getPtrTy(), valOutSlot);
                                llvm::Value* vTyped = builder.CreateLoad(loomType(valType), vRaw);
                                builder.CreateStore(vTyped, valVarSlot);
                            }

                            loops.push_back({exit, header, true, activeEnsures.size()});
                            emitStatements(s.body);
                            loops.pop_back();
                            if (reachable && !terminated()) builder.CreateBr(header);

                            builder.SetInsertPoint(exit);
                            reachable = true;
                        } else {
                            Type elemType = (collType.kind == TypeKind::Text) ? Type::character() : *collType.pointeeType;
                            llvm::Value* lenVal = nullptr;
                            llvm::Value* baseAddr = nullptr;
                            llvm::Value* sliceVal = nullptr;

                            if (collType.kind == TypeKind::Array) {
                                lenVal = builder.getInt64(collType.arraySize);
                                baseAddr = address(*s.expr);
                            } else if (collType.kind == TypeKind::Slice) {
                                sliceVal = emit(*s.expr);
                                lenVal = builder.CreateExtractValue(sliceVal, 1, "slice.len");
                            } else if (collType.kind == TypeKind::Vector) {
                                baseAddr = address(*s.expr);
                                auto* lenPtr = builder.CreateStructGEP(loomType(collType), baseAddr, 1);
                                lenVal = builder.CreateLoad(builder.getInt64Ty(), lenPtr, "vec.len");
                            } else if (collType.kind == TypeKind::Text) {
                                baseAddr = address(*s.expr);
                                auto* lenPtr = builder.CreateStructGEP(loomType(collType), baseAddr, 1);
                                lenVal = builder.CreateLoad(builder.getInt64Ty(), lenPtr, "text.len");
                            }

                            auto* varSlot = builder.CreateAlloca(loomType(elemType), nullptr, s.loopVar + ".addr");
                            storage[s.loopVar] = varSlot;
                            types[s.loopVar] = elemType;

                            llvm::Value* idxVarSlot = nullptr;
                            if (!s.loopIndexVar.empty()) {
                                idxVarSlot = builder.CreateAlloca(builder.getInt64Ty(), nullptr, s.loopIndexVar + ".addr");
                                storage[s.loopIndexVar] = idxVarSlot;
                                types[s.loopIndexVar] = Type::unsignedInt(64);
                            }

                            auto* iSlot = builder.CreateAlloca(builder.getInt64Ty(), nullptr, "coll.for.i");
                            builder.CreateStore(builder.getInt64(1), iSlot);

                            auto* header = llvm::BasicBlock::Create(context, "coll.for.header", current);
                            auto* body = llvm::BasicBlock::Create(context, "coll.for.body", current);
                            auto* latch = llvm::BasicBlock::Create(context, "coll.for.latch", current);
                            auto* exit = llvm::BasicBlock::Create(context, "coll.for.exit", current);

                            builder.CreateBr(header);
                            builder.SetInsertPoint(header);
                            reachable = true;

                            llvm::Value* curI = builder.CreateLoad(builder.getInt64Ty(), iSlot);
                            llvm::Value* cond = builder.CreateICmpSLE(curI, lenVal);
                            builder.CreateCondBr(cond, body, exit);

                            builder.SetInsertPoint(body);
                            reachable = true;
                            if (idxVarSlot) builder.CreateStore(curI, idxVarSlot);

                            llvm::Value* i0 = builder.CreateSub(curI, builder.getInt64(1));
                            llvm::Value* elemVal = nullptr;
                            if (collType.kind == TypeKind::Array) {
                                auto* elemPtr = builder.CreateInBoundsGEP(loomType(collType), baseAddr, {builder.getInt64(0), i0});
                                elemVal = builder.CreateLoad(loomType(elemType), elemPtr);
                            } else if (collType.kind == TypeKind::Slice) {
                                llvm::Value* dataPtr = builder.CreateExtractValue(sliceVal, 0);
                                auto* elemPtr = builder.CreateInBoundsGEP(loomType(elemType), dataPtr, {i0});
                                elemVal = builder.CreateLoad(loomType(elemType), elemPtr);
                            } else if (collType.kind == TypeKind::Vector) {
                                auto* getFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false);
                                auto getCallee = module->getOrInsertFunction("loom_vec_get", getFnTy);
                                llvm::Value* elemPtr = builder.CreateCall(getCallee, {baseAddr, curI});
                                elemVal = builder.CreateLoad(loomType(elemType), elemPtr);
                            } else if (collType.kind == TypeKind::Text) {
                                auto* dataPtrField = builder.CreateStructGEP(loomType(collType), baseAddr, 0);
                                llvm::Value* dataPtr = builder.CreateLoad(builder.getPtrTy(), dataPtrField);
                                auto* charPtr = builder.CreateInBoundsGEP(builder.getInt8Ty(), dataPtr, {i0});
                                llvm::Value* b = builder.CreateLoad(builder.getInt8Ty(), charPtr);
                                elemVal = builder.CreateZExt(b, builder.getInt32Ty());
                            }
                            builder.CreateStore(elemVal, varSlot);

                            loops.push_back({exit, latch, true, activeEnsures.size()});
                            emitStatements(s.body);
                            loops.pop_back();
                            if (reachable && !terminated()) builder.CreateBr(latch);

                            builder.SetInsertPoint(latch);
                            reachable = true;
                            llvm::Value* nxt = builder.CreateAdd(curI, builder.getInt64(1));
                            builder.CreateStore(nxt, iSlot);
                            builder.CreateBr(header);

                            builder.SetInsertPoint(exit);
                            reachable = true;
                        }
                    }
                    break;
                }
                case Stmt::Kind::Match: {
                    Type exprType = analyzer.typeOf(*s.expr);
                    bool hasComplex = (exprType.kind == TypeKind::Tuple || exprType.kind == TypeKind::Record || exprType.kind == TypeKind::String);
                    if (!hasComplex) {
                        for (const auto& mc : s.matchCases) {
                            if (mc.isRange || mc.guard || mc.isDefault ||
                                (mc.pattern && mc.pattern->kind == Expr::Kind::Variable && !globalStorage.count(mc.pattern->text) && !types.count(mc.pattern->text))) {
                                hasComplex = true;
                                break;
                            }
                        }
                    }
                    if (!hasComplex && (exprType.integer() || exprType.kind == TypeKind::Enum || exprType.kind == TypeKind::Boolean || exprType.kind == TypeKind::Character)) {
                        llvm::Value* condVal = emit(*s.expr);
                        auto* endBB = llvm::BasicBlock::Create(context, "match.end", current);
                        auto* defaultBB = s.elseBody.empty() ? endBB : llvm::BasicBlock::Create(context, "match.else", current);
                        auto* switchInst = builder.CreateSwitch(condVal, defaultBB, s.matchCases.size());
                        bool anyFallthrough = s.elseBody.empty();
                        for (const auto& mc : s.matchCases) {
                            auto* caseBB = llvm::BasicBlock::Create(context, "match.case", current);
                            std::uint64_t val = 0;
                            if (mc.pattern->kind == Expr::Kind::EnumVariant) {
                                auto ev = analyzer.enumValueOf(*mc.pattern);
                                val = ev.negative ? std::uint64_t(0) - ev.magnitude : ev.magnitude;
                            } else if (mc.pattern->kind == Expr::Kind::Integer) {
                                val = mc.pattern->magnitude;
                            } else if (mc.pattern->kind == Expr::Kind::Unary && mc.pattern->text == "-" &&
                                       mc.pattern->left && mc.pattern->left->kind == Expr::Kind::Integer) {
                                val = std::uint64_t(0) - mc.pattern->left->magnitude;
                            } else if (mc.pattern->kind == Expr::Kind::Character) {
                                val = decodeUtf8Scalar(mc.pattern->text, mc.pattern->line);
                            } else if (mc.pattern->kind == Expr::Kind::Boolean) {
                                val = mc.pattern->boolean ? 1 : 0;
                            } else {
                                unsigned steps = 0;
                                auto cv = constEvaluator ? constEvaluator->evalExpr(*mc.pattern, {}, 0, steps) : ConstVal{};
                                if (cv.isKnown()) {
                                    val = cv.isNegative ? std::uint64_t(0) - cv.intVal : cv.intVal;
                                }
                            }
                            auto* constVal = llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(condVal->getType()), val);
                            switchInst->addCase(constVal, caseBB);
                            builder.SetInsertPoint(caseBB);
                            reachable = true;
                            emitStatements(mc.body);
                            if (reachable && !terminated()) {
                                builder.CreateBr(endBB);
                                anyFallthrough = true;
                            }
                        }
                        if (!s.elseBody.empty()) {
                            builder.SetInsertPoint(defaultBB);
                            reachable = true;
                            emitStatements(s.elseBody);
                            if (reachable && !terminated()) {
                                builder.CreateBr(endBB);
                                anyFallthrough = true;
                            }
                        }
                        if (anyFallthrough) {
                            builder.SetInsertPoint(endBB);
                            reachable = true;
                        } else {
                            endBB->eraseFromParent();
                            builder.ClearInsertionPoint();
                            reachable = false;
                        }
                    } else {
                        llvm::Value* condVal = emit(*s.expr);
                        auto* endBB = llvm::BasicBlock::Create(context, "match.end", current);
                        auto* defaultBB = s.elseBody.empty() ? endBB : llvm::BasicBlock::Create(context, "match.else", current);
                        bool hasWildcard = false;
                        for (const auto& mc : s.matchCases) {
                            if (mc.isWildcard || mc.isDefault) hasWildcard = true;
                        }
                        bool anyFallthrough = s.elseBody.empty() && !hasWildcard;
                        for (size_t ci = 0; ci < s.matchCases.size(); ++ci) {
                            const auto& mc = s.matchCases[ci];
                            auto savedStorage = storage;
                            auto savedTypes = types;
                            auto* caseBB = llvm::BasicBlock::Create(context, "match.case", current);
                            auto* nextBB = (ci + 1 < s.matchCases.size()) ?
                                llvm::BasicBlock::Create(context, "match.check", current) : defaultBB;
                            llvm::Value* isMatch = nullptr;
                            if (mc.isWildcard || mc.isDefault) {
                                isMatch = builder.getInt1(true);
                            } else if (mc.isRange) {
                                llvm::Value* startVal = emit(*mc.pattern);
                                llvm::Value* endVal = emit(*mc.rangeEnd);
                                if (exprType.kind == TypeKind::Signed) {
                                    llvm::Value* ge = builder.CreateICmpSGE(condVal, startVal);
                                    llvm::Value* le = builder.CreateICmpSLE(condVal, endVal);
                                    isMatch = builder.CreateAnd(ge, le);
                                } else {
                                    llvm::Value* ge = builder.CreateICmpUGE(condVal, startVal);
                                    llvm::Value* le = builder.CreateICmpULE(condVal, endVal);
                                    isMatch = builder.CreateAnd(ge, le);
                                }
                            } else if (exprType.kind == TypeKind::Tuple) {
                                isMatch = builder.getInt1(true);
                                for (size_t ti = 0; ti < mc.pattern->arguments.size(); ++ti) {
                                    llvm::Value* condElem = builder.CreateExtractValue(condVal, static_cast<unsigned>(ti));
                                    const auto& arg = *mc.pattern->arguments[ti];
                                    if (arg.kind == Expr::Kind::Variable &&
                                        !globalStorage.count(arg.text) &&
                                        !savedTypes.count(arg.text)) {
                                        auto* slot = builder.CreateAlloca(loomType(exprType.tupleTypes[ti]), nullptr, arg.text + ".addr");
                                        builder.CreateStore(condElem, slot);
                                        storage[arg.text] = slot;
                                        types[arg.text] = exprType.tupleTypes[ti];
                                    } else {
                                        llvm::Value* patElem = emit(arg);
                                        llvm::Value* cmp = condElem->getType()->isFloatingPointTy() ?
                                            builder.CreateFCmpOEQ(condElem, patElem) :
                                            builder.CreateICmpEQ(condElem, patElem);
                                        isMatch = builder.CreateAnd(isMatch, cmp);
                                    }
                                }
                            } else if (exprType.kind == TypeKind::Record) {
                                isMatch = builder.getInt1(true);
                                for (size_t fi = 0; fi < mc.pattern->fieldNames.size(); ++fi) {
                                    const std::string& fname = mc.pattern->fieldNames[fi];
                                    unsigned fieldIdx = analyzer.fieldIndex(exprType.name, fname);
                                    llvm::Value* condField = builder.CreateExtractValue(condVal, fieldIdx);
                                    const auto& arg = *mc.pattern->arguments[fi];
                                    Type fieldType = analyzer.fieldType(exprType.name, fname);
                                    if (arg.kind == Expr::Kind::Variable &&
                                        !globalStorage.count(arg.text) &&
                                        !savedTypes.count(arg.text)) {
                                        auto* slot = builder.CreateAlloca(loomType(fieldType), nullptr, arg.text + ".addr");
                                        builder.CreateStore(condField, slot);
                                        storage[arg.text] = slot;
                                        types[arg.text] = fieldType;
                                    } else {
                                        llvm::Value* patField = emit(arg);
                                        llvm::Value* cmp = condField->getType()->isFloatingPointTy() ?
                                            builder.CreateFCmpOEQ(condField, patField) :
                                            builder.CreateICmpEQ(condField, patField);
                                        isMatch = builder.CreateAnd(isMatch, cmp);
                                    }
                                }
                            } else if (mc.pattern->kind == Expr::Kind::Variable &&
                                       !globalStorage.count(mc.pattern->text) &&
                                       !savedTypes.count(mc.pattern->text)) {
                                auto* slot = builder.CreateAlloca(loomType(exprType), nullptr, mc.pattern->text + ".addr");
                                builder.CreateStore(condVal, slot);
                                storage[mc.pattern->text] = slot;
                                types[mc.pattern->text] = exprType;
                                isMatch = builder.getInt1(true);
                            } else {
                                llvm::Value* patVal = emit(*mc.pattern);
                                if (exprType.kind == TypeKind::String) {
                                    auto* isNull = builder.CreateICmpEQ(condVal, llvm::ConstantPointerNull::get(builder.getPtrTy()));
                                    auto strcmpFn = module->getOrInsertFunction("strcmp",
                                        llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy(), builder.getPtrTy()}, false));
                                    auto* nonNullBB = llvm::BasicBlock::Create(context, "match.strcmp", current);
                                    auto* doneCmpBB = llvm::BasicBlock::Create(context, "match.cmpdone", current);
                                    builder.CreateCondBr(isNull, doneCmpBB, nonNullBB);
                                    auto* prevBB = builder.GetInsertBlock();
                                    builder.SetInsertPoint(nonNullBB);
                                    llvm::Value* cmpRes = builder.CreateCall(strcmpFn, {condVal, patVal});
                                    llvm::Value* eqZero = builder.CreateICmpEQ(cmpRes, builder.getInt32(0));
                                    builder.CreateBr(doneCmpBB);
                                    auto* finalBB = builder.GetInsertBlock();
                                    builder.SetInsertPoint(doneCmpBB);
                                    auto* phi = builder.CreatePHI(builder.getInt1Ty(), 2, "strmatch");
                                    phi->addIncoming(builder.getInt1(false), prevBB);
                                    phi->addIncoming(eqZero, finalBB);
                                    isMatch = phi;
                                } else if (condVal->getType()->isFloatingPointTy())
                                    isMatch = builder.CreateFCmpOEQ(condVal, patVal);
                                else
                                    isMatch = builder.CreateICmpEQ(condVal, patVal);
                            }
                            if (mc.guard) {
                                llvm::Value* gVal = emit(*mc.guard);
                                if (gVal->getType() != builder.getInt1Ty()) gVal = builder.CreateICmpNE(gVal, llvm::Constant::getNullValue(gVal->getType()));
                                isMatch = builder.CreateAnd(isMatch, gVal);
                            }
                            if ((mc.isWildcard || mc.isDefault) && !mc.guard) {
                                builder.CreateBr(caseBB);
                            } else {
                                builder.CreateCondBr(isMatch, caseBB, nextBB);
                            }
                            builder.SetInsertPoint(caseBB);
                            reachable = true;
                            emitStatements(mc.body);
                            if (reachable && !terminated()) {
                                builder.CreateBr(endBB);
                                anyFallthrough = true;
                            }
                            storage = std::move(savedStorage);
                            types = std::move(savedTypes);
                            if (nextBB != defaultBB) {
                                builder.SetInsertPoint(nextBB);
                            }
                        }
                        if (!s.elseBody.empty()) {
                            builder.SetInsertPoint(defaultBB);
                            reachable = true;
                            emitStatements(s.elseBody);
                            if (reachable && !terminated()) {
                                builder.CreateBr(endBB);
                                anyFallthrough = true;
                            }
                        }
                        if (anyFallthrough) {
                            builder.SetInsertPoint(endBB);
                            reachable = true;
                        } else {
                            endBB->eraseFromParent();
                            builder.ClearInsertionPoint();
                            reachable = false;
                        }
                    }
                    break;
                }
                case Stmt::Kind::Lock: {
                    llvm::Value* mtxVal = emit(*s.expr);
                    auto* lockFnTy = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false);
                    auto lockCallee = module->getOrInsertFunction("loom_mutex_lock", lockFnTy);
                    auto unlockCallee = module->getOrInsertFunction("loom_mutex_unlock", lockFnTy);
                    builder.CreateCall(lockCallee, {mtxVal});
                    emitStatements(s.body);
                    if (reachable && !terminated()) {
                        builder.CreateCall(unlockCallee, {mtxVal});
                    }
                    break;
                }
            }
        }
        if (reachable && !terminated()) {
            for (std::size_t i = activeEnsures.size(); i > ensureBase; --i) {
                emitEnsureStmt(*activeEnsures[i - 1]);
            }
        }
        activeEnsures.resize(ensureBase);
    }

    llvm::Constant* emitGlobalConstant(const Expr& e, Type t) {
        if (t.kind == TypeKind::Record) {
            auto* structTy = llvm::cast<llvm::StructType>(loomType(t));
            if (e.kind == Expr::Kind::RecordLiteral) {
                const auto& order = analyzer.fieldNames(t.name);
                std::unordered_map<std::string, const Expr*> litMap;
                for (std::size_t i = 0; i < e.fieldNames.size(); ++i) {
                    litMap[e.fieldNames[i]] = e.arguments[i].get();
                }
                std::vector<llvm::Constant*> elements;
                for (const auto& fn : order) {
                    Type ft = analyzer.fieldType(t.name, fn);
                    auto it = litMap.find(fn);
                    if (it != litMap.end()) {
                        elements.push_back(emitGlobalConstant(*it->second, ft));
                    } else {
                        elements.push_back(llvm::Constant::getNullValue(loomType(ft)));
                    }
                }
                return llvm::ConstantStruct::get(structTy, elements);
            }
            return llvm::Constant::getNullValue(structTy);
        }
        if (t.kind == TypeKind::Array) {
            auto* arrTy = llvm::cast<llvm::ArrayType>(loomType(t));
            std::vector<llvm::Constant*> elements;
            Type elemType = *t.pointeeType;
            if (e.kind == Expr::Kind::ArrayLiteral) {
                for (const auto& arg : e.arguments) {
                    elements.push_back(emitGlobalConstant(*arg, elemType));
                }
            } else {
                return llvm::Constant::getNullValue(arrTy);
            }
            return llvm::ConstantArray::get(arrTy, elements);
        }
        if (e.kind == Expr::Kind::Integer)
            return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), e.magnitude);
        if (e.kind == Expr::Kind::Unary && e.text == "-") {
            if (t.kind == TypeKind::Signed || t.kind == TypeKind::Unsigned) {
                if (e.left && e.left->kind == Expr::Kind::Integer) {
                    uint64_t val = 0 - e.left->magnitude;
                    return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), val);
                }
            } else if (t.kind == TypeKind::Float) {
                if (e.left && e.left->kind == Expr::Kind::Floating) {
                    return llvm::ConstantFP::get(loomType(t), -e.left->floating);
                }
            }
        }
        if (e.kind == Expr::Kind::Boolean)
            return llvm::ConstantInt::get(loomType(t), e.boolean);
        if (e.kind == Expr::Kind::Floating)
            return llvm::ConstantFP::get(loomType(t), e.floating);
        if (e.kind == Expr::Kind::Character)
            return llvm::ConstantInt::get(loomType(t), decodeUtf8Scalar(e.text, e.line));
        if (e.kind == Expr::Kind::EnumVariant) {
            const auto value = analyzer.enumValueOf(e);
            const std::uint64_t bits = value.negative ? std::uint64_t(0) - value.magnitude : value.magnitude;
            return llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(loomType(t)), bits);
        }
        if (e.kind == Expr::Kind::String) {
            auto* data = llvm::ConstantDataArray::getString(context, e.text, true);
            auto* literal = new llvm::GlobalVariable(*module, data->getType(), true,
                llvm::GlobalValue::PrivateLinkage, data, ".loom.global.string." + std::to_string(++stringCounter));
            llvm::Constant* indices[] = {llvm::ConstantInt::get(builder.getInt32Ty(), 0),
                                          llvm::ConstantInt::get(builder.getInt32Ty(), 0)};
            return llvm::ConstantExpr::getInBoundsGetElementPtr(data->getType(), literal, indices);
        }
        return llvm::Constant::getNullValue(loomType(t));
    }

  public:
    Generator(const std::string& moduleName, const Analyzer& a, bool release = false, OptReport* rep = nullptr, bool bm = false, std::shared_ptr<TargetProfile> tp = nullptr, bool emitDbg = false)
        : module(std::make_unique<llvm::Module>(moduleName, context)), sourcePath(moduleName), analyzer(a),
          releaseMode(release), targetProfile(tp ? tp : createTargetProfile(bm)), baremetal(tp ? !tp->supportsHeap() : bm), optReport(rep), emitDebug(emitDbg) {
        if (emitDebug) {
            dibuilder = std::make_unique<llvm::DIBuilder>(*module);
            std::filesystem::path p(moduleName);
            std::string dir = p.parent_path().empty() ? "." : p.parent_path().string();
            std::string filename = p.filename().string();
            diFile = dibuilder->createFile(filename, dir);
            diCompileUnit = dibuilder->createCompileUnit(
                llvm::dwarf::DW_LANG_C,
                diFile,
                "Loom Compiler 1.4.0",
                release,
                "",
                0
            );
            module->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
#if defined(__APPLE__)
            module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
#else
            module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
#endif
        }
    }
    std::unique_ptr<llvm::Module> run(const std::vector<Function>& fs, const std::vector<VarDecl>& globals = {},
                                      const std::vector<RecordDecl>& records = {}, bool nativeEntry = false) {
        constEvaluator = std::make_unique<ConstEvaluator>(analyzer, fs, globals, records, optReport);
        {
            writeCharacterFunction = createCharacterWriter();
        }
        recordStorageTypes.clear();
        recordDecls.clear();
        functionDecls.clear();
        for (const auto& f : fs) {
            functionDecls.emplace(f.name, &f);
        }
        for (const auto& record : records) {
            recordDecls.emplace(record.name, &record);
            recordStorageTypes.emplace(record.name, llvm::StructType::create(context, record.name));
        }
        for (const auto& record : records) {
            std::vector<llvm::Type*> fields;
            for (const auto& field : record.fields) fields.push_back(loomType(field.type));
            recordStorageTypes.at(record.name)->setBody(fields, record.isPacked);
        }
        for (const auto& r : records) {
            if (r.hasDrop) {
                std::string dropFnName = "__loom_drop_" + r.name;
                auto* dropType = llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false);
                auto* dropFn = llvm::Function::Create(dropType, llvm::Function::InternalLinkage, dropFnName, *module);
                auto* prevCurrent = current;
                current = dropFn;
                auto* entry = llvm::BasicBlock::Create(context, "entry", dropFn);
                auto* prevInsert = builder.GetInsertBlock();
                builder.SetInsertPoint(entry);
                reachable = true;
                auto prevStorage = storage;
                auto prevTypes = types;
                auto* selfPtr = dropFn->getArg(0);
                selfPtr->setName("self");
                for (size_t fi = 0; fi < r.fields.size(); ++fi) {
                    const auto& fld = r.fields[fi];
                    auto* fieldPtr = builder.CreateStructGEP(loomType(Type::recordType(r.name)), selfPtr, static_cast<unsigned>(fi), fld.name + ".addr");
                    storage[fld.name] = fieldPtr;
                    types[fld.name] = fld.type;
                }
                emitStatements(r.dropBody);
                if (reachable && !terminated()) builder.CreateRetVoid();
                storage = std::move(prevStorage);
                types = std::move(prevTypes);
                current = prevCurrent;
                if (prevInsert) builder.SetInsertPoint(prevInsert);
            }
        }
        globalStorage.clear();
        for (const auto& g : globals) {
            llvm::Constant* initial = g.initializer ? emitGlobalConstant(*g.initializer, g.type)
                                                   : llvm::Constant::getNullValue(loomType(g.type));
            auto* variable = new llvm::GlobalVariable(*module, loomType(g.type), g.constant,
                llvm::GlobalValue::ExternalLinkage, initial, g.name);
            globalStorage.emplace(g.name, variable);
        }
        // All signatures are declared first, so LLVM symbols remain stable and future calls can be added safely.
        functions.clear();
        for (const auto& f : fs) {
            std::vector<llvm::Type*> args;
            for (const auto& p : f.params) {
                if (p.isMut || p.isRef)
                    args.push_back(builder.getPtrTy());
                else
                    args.push_back(loomType(p.type));
            }
            llvm::Type* retTy = llvm::Type::getVoidTy(context);
            if (f.errorResult) {
                std::vector<llvm::Type*> unionFields;
                unionFields.push_back(loomType(*f.errorResult));
                if (f.result) {
                    Type valTy = *f.result;
                    valTy.errorType = nullptr;
                    unionFields.push_back(loomType(valTy));
                }
                retTy = llvm::StructType::get(context, unionFields);
            } else if (f.result) {
                retTy = loomType(*f.result);
            }
            auto* ft = llvm::FunctionType::get(retTy, args, false);
            auto* fn = module->getFunction(f.name);
            if (!fn) {
                fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, f.name, *module);
                fn->addFnAttr(llvm::Attribute::NoUnwind);
            }
            functions.emplace(f.name, fn);
        }
        for (const auto& f : fs) {
            if (f.isExtern) continue;
            current = functions.at(f.name);
            currentFn = &f;
            if (emitDebug && dibuilder && diFile) {
                llvm::DISubroutineType* diSubroutine = dibuilder->createSubroutineType(dibuilder->getOrCreateTypeArray({}));
                llvm::DISubprogram* diSubprogram = dibuilder->createFunction(
                    diFile,
                    f.name,
                    f.name,
                    diFile,
                    f.line > 0 ? f.line : 1,
                    diSubroutine,
                    f.line > 0 ? f.line : 1,
                    llvm::DINode::FlagPrototyped,
                    llvm::DISubprogram::SPFlagDefinition
                );
                current->setSubprogram(diSubprogram);
                diScopes.push_back(diSubprogram);
                builder.SetCurrentDebugLocation(llvm::DILocation::get(context, f.line > 0 ? f.line : 1, 1, diSubprogram));
            } else {
                builder.SetCurrentDebugLocation(llvm::DebugLoc());
            }
            auto* entry = llvm::BasicBlock::Create(context, "entry", current);
            builder.SetInsertPoint(entry);
            reachable = true;
            storage.clear();
            types.clear();
            loops.clear();
            activeEnsures.clear();
            std::size_t index = 0;
            for (auto& arg : current->args()) {
                const auto& p = f.params[index++];
                arg.setName(p.name);
                types.emplace(p.name, p.type);
                if (p.isMut || p.isRef) {
                    storage.emplace(p.name, &arg);
                } else {
                    auto* slot = builder.CreateAlloca(loomType(p.type), nullptr, p.name + ".addr");
                    builder.CreateStore(&arg, slot);
                    storage.emplace(p.name, slot);
                }
            }
            currentFunctionDrops.clear();
            for (const auto& d : f.locals) {
                if (d.type.kind == TypeKind::Vector || d.type.kind == TypeKind::Text ||
                    d.type.kind == TypeKind::HMap || d.type.kind == TypeKind::FileHandle) {
                    currentFunctionDrops.push_back({d.name, d.type});
                } else if (d.type.kind == TypeKind::Record) {
                    auto it = recordDecls.find(d.type.name);
                    if (it != recordDecls.end() && it->second->hasDrop) {
                        currentFunctionDrops.push_back({d.name, d.type});
                    }
                }
            }
            for (const auto& d : f.locals) {
                types.emplace(d.name, d.type);
                auto* slot = builder.CreateAlloca(loomType(d.type), nullptr, d.name + ".addr");
                builder.CreateStore(llvm::Constant::getNullValue(loomType(d.type)), slot);
                storage.emplace(d.name, slot);
                if (d.type.kind == TypeKind::Text && !d.initializer) {
                    uint64_t cap = d.type.arraySize > 0 ? d.type.arraySize : 64;
                    auto initFn = module->getOrInsertFunction("loom_text_init",
                        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty()}, false));
                    builder.CreateCall(initFn, {slot, builder.getInt64(cap)});
                } else if (d.type.kind == TypeKind::Vector && !d.initializer) {
                    uint64_t count = d.type.arraySize;
                    Type elemType = *d.type.pointeeType;
                    const auto& layout = module->getDataLayout();
                    uint64_t elemSize = layout.getTypeAllocSize(loomType(elemType));
                    uint64_t elemAlign = layout.getABITypeAlign(loomType(elemType)).value();
                    auto initFn = module->getOrInsertFunction("loom_vec_init",
                        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy(), builder.getInt64Ty(), builder.getInt32Ty(), builder.getInt32Ty()}, false));
                    builder.CreateCall(initFn, {slot, builder.getInt64(count), builder.getInt32(elemSize), builder.getInt32(elemAlign)});
                } else if (d.type.kind == TypeKind::HMap && !d.initializer) {
                    Type keyType = *d.type.keyType;
                    Type valType = *d.type.valueType;
                    uint32_t keyKind = (keyType.kind == TypeKind::String) ? 1 : 0;
                    const auto& layout = module->getDataLayout();
                    uint32_t valSize = layout.getTypeAllocSize(loomType(valType));
                    uint32_t valAlign = layout.getABITypeAlign(loomType(valType)).value();
                    auto* createFnTy = llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt32Ty()}, false);
                    auto createCallee = module->getOrInsertFunction("loom_hmap_create", createFnTy);
                    llvm::Value* mapPtr = builder.CreateCall(createCallee, {builder.getInt32(keyKind), builder.getInt32(valSize), builder.getInt32(valAlign)}, "hmap.init");
                    builder.CreateStore(mapPtr, slot);
                }
            }
            for (const auto& d : f.locals)
                if (d.initializer) builder.CreateStore(emit(*d.initializer), storage.at(d.name));
            emitStatements(f.body);
            if (reachable && !terminated()) {
                if (f.result)
                    throw Error("line " + std::to_string(f.line) +
                                ": internal flow-analysis mismatch: value block reaches end");
                for (auto it = activeEnsures.rbegin(); it != activeEnsures.rend(); ++it)
                    emitEnsureStmt(**it);
                emitScopeDrops();
                builder.CreateRetVoid();
            }
            if (emitDebug && dibuilder && !diScopes.empty()) {
                diScopes.pop_back();
            }
            builder.SetCurrentDebugLocation(llvm::DebugLoc());
            if (llvm::verifyFunction(*current, &llvm::errs()))
                throw Error("LLVM function verification failed for '" + f.name + "'");
        }
        if (nativeEntry) {
            builder.SetCurrentDebugLocation(llvm::DebugLoc());
            auto* loomMain = module->getFunction("Main");
            if (!loomMain || !loomMain->arg_empty() || !loomMain->getReturnType()->isVoidTy())
                throw Error("build.main must define a parameterless procedure named 'Main' (Block Main ... End Main)");
            auto* nativeType = llvm::FunctionType::get(builder.getInt32Ty(), false);
            auto* nativeMain = llvm::Function::Create(nativeType, llvm::Function::ExternalLinkage, "main", *module);
            auto* entry = llvm::BasicBlock::Create(context, "entry", nativeMain);
            builder.SetInsertPoint(entry);
            builder.CreateCall(loomMain);
            builder.CreateRet(builder.getInt32(0));
            builder.ClearInsertionPoint();
        }
        if (llvm::verifyModule(*module, &llvm::errs()))
            throw Error("LLVM module verification failed");
        if (emitDebug && dibuilder) {
            dibuilder->finalize();
        }
        return std::move(module);
    }
};

std::string diagnosticCode(const std::string& message) {
    if (message.find("unsupported character") != std::string::npos || message.find("literal is too large") != std::string::npos ||
        message.find("unterminated") != std::string::npos || message.find("escape") != std::string::npos ||
        message.find("UTF-8") != std::string::npos || message.find("Unicode scalar") != std::string::npos ||
        message.find("floating-point literal is out of range") != std::string::npos) return "E0001";
    if (message.find("expected ") != std::string::npos || message.find("mismatched") != std::string::npos ||
        message.find("unexpected End") != std::string::npos || message.find("unsupported keyword") != std::string::npos) return "E1001";
    return "E2001";
}
std::string diagnosticTitle(std::string message) {
    const auto prefix = std::regex(R"(^line [0-9]+:\s*)");
    message = std::regex_replace(message, prefix, "");
    if (!message.empty()) message[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(message[0])));
    return message;
}
void printDiagnostic(const Error& error, const std::vector<std::string>& paths,
                     const std::vector<std::string>& sourceTexts) {
    std::string message = error.what();
    int line = error.span.line;
    if (line <= 0) {
        std::smatch match;
        if (std::regex_search(message, match, std::regex(R"(^line ([0-9]+):)"))) line = std::stoi(match[1]);
    }
    const std::size_t source = error.span.source;
    const bool hasSource = source < paths.size() && source < sourceTexts.size() && line > 0;
    const std::string title = diagnosticTitle(message);
    bool useColor = false;
#ifndef _WIN32
    if ((isatty(STDERR_FILENO) || std::getenv("LOOM_COLOR") != nullptr) && std::getenv("NO_COLOR") == nullptr) useColor = true;
#endif
    const std::string red = useColor ? "\033[1;31m" : "";
    const std::string bold = useColor ? "\033[1m" : "";
    const std::string blue = useColor ? "\033[1;34m" : "";
    const std::string cyan = useColor ? "\033[1;36m" : "";
    const std::string yellow = useColor ? "\033[1;33m" : "";
    const std::string reset = useColor ? "\033[0m" : "";

    std::cerr << red << "error[" << diagnosticCode(message) << "]: " << reset << bold << title << reset << "\n";
    if (!hasSource) {
        if (!paths.empty()) std::cerr << "  " << blue << "-->" << reset << " " << paths.front() << "\n";
        return;
    }
    std::vector<std::string> lines;
    std::istringstream input(sourceTexts[source]);
    std::string text;
    while (std::getline(input, text)) lines.push_back(text);
    std::cerr << "  " << blue << "-->" << reset << " " << paths[source] << ":" << line << ":";
    std::string sourceLine;
    if (static_cast<std::size_t>(line) <= lines.size()) sourceLine = lines[static_cast<std::size_t>(line - 1)];
    int column = std::max(1, error.span.column);
    if (error.span.column == 0 && !sourceLine.empty()) {
        std::smatch match;
        if (std::regex_search(message, match, std::regex("'([^']+)'"))) {
            const auto found = sourceLine.find(match[1].str());
            if (found != std::string::npos) {
                column = 1;
                for (std::size_t i = 0; i < found; ++i) {
                    const auto byte = static_cast<unsigned char>(sourceLine[i]);
                    if (sourceLine[i] == '\t') column += 4 - ((column - 1) % 4);
                    else if ((byte & 0xC0) != 0x80) column += (byte & 0xF8) == 0xF0 ? 2 : 1;
                }
            }
        }
    }
    std::cerr << column << "\n   " << blue << "|" << reset << "\n"
              << blue << std::setw(3) << line << " | " << reset << sourceLine << "\n   "
              << blue << "|" << reset << " ";
    for (int i = 1; i < column; ++i) std::cerr << ' ';
    std::cerr << red << '^';
    for (int i = 1; i < std::max(1, error.span.length); ++i) std::cerr << '~';
    std::cerr << " " << title << reset << "\n";
    if (message.find("unsupported character") != std::string::npos)
        std::cerr << "   " << cyan << "= help:" << reset << " check that the character is part of the currently supported Loom syntax\n";
}
std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        throw Error("cannot open input file '" + path + "'");
    std::ostringstream contents;
    contents << in.rdbuf();
    if (in.bad())
        throw Error("failed while reading input file '" + path + "'");
    return contents.str();
}
void optimizeModule(llvm::Module& module, llvm::TargetMachine& machine, bool aggressive, bool sizeOpt = false) {
    if (!aggressive) return;
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    llvm::PipelineTuningOptions PTO;
    if (sizeOpt) {
        PTO.LoopVectorization = false;
        PTO.SLPVectorization = false;
        PTO.LoopUnrolling = false;
        PTO.LoopInterleaving = false;
        PTO.MergeFunctions = true;
    } else {
        PTO.LoopVectorization = true;
        PTO.SLPVectorization = true;
        PTO.LoopUnrolling = true;
        PTO.LoopInterleaving = true;
        PTO.MergeFunctions = true;
    }
    llvm::PassBuilder PB(&machine, PTO);
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(
        sizeOpt ? llvm::OptimizationLevel::O2 : llvm::OptimizationLevel::O3);
    MPM.run(module, MAM);
}

extern "C" {
    void LLVMInitializeAArch64TargetInfo();
    void LLVMInitializeAArch64Target();
    void LLVMInitializeAArch64TargetMC();
    void LLVMInitializeAArch64AsmPrinter();
    void LLVMInitializeAArch64AsmParser();
    void LLVMInitializeARMTargetInfo();
    void LLVMInitializeARMTarget();
    void LLVMInitializeARMTargetMC();
    void LLVMInitializeARMAsmPrinter();
    void LLVMInitializeARMAsmParser();
    void LLVMInitializeX86TargetInfo();
    void LLVMInitializeX86Target();
    void LLVMInitializeX86TargetMC();
    void LLVMInitializeX86AsmPrinter();
    void LLVMInitializeX86AsmParser();
}

inline void initializeLoomTargets() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    (void)llvm::InitializeNativeTarget();
    (void)llvm::InitializeNativeTargetAsmPrinter();
    (void)llvm::InitializeNativeTargetAsmParser();
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeAArch64AsmParser();
    LLVMInitializeARMTargetInfo();
    LLVMInitializeARMTarget();
    LLVMInitializeARMTargetMC();
    LLVMInitializeARMAsmPrinter();
    LLVMInitializeARMAsmParser();
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser();
}

void writeObject(llvm::Module& module, const std::string& path, bool release,
                 const std::string& customTriple = "", const std::string& customCpu = "",
                 bool baremetal = false, OptReport* report = nullptr, bool sizeOpt = false) {
    initializeLoomTargets();

    TargetConfig cfg = TargetConfig::resolve(release, customTriple, customCpu, baremetal, sizeOpt);
    std::string error;
    auto machine = cfg.createMachine(error);
    if (!machine) throw Error("cannot create LLVM target machine: " + error);

    if (report) {
        report->targetTriple = cfg.triple;
        report->arch = cfg.archName();
        report->osPlatform = cfg.osName();
        report->cpu = cfg.cpu;
        report->features = cfg.features;
    }
    module.setTargetTriple(machine->getTargetTriple());
    module.setDataLayout(machine->createDataLayout());

    if (release) {
        optimizeModule(module, *machine, true, sizeOpt);
    }

    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec);
    if (ec) throw Error("cannot write object file '" + path + "': " + ec.message());
    llvm::legacy::PassManager passes;
    if (machine->addPassesToEmitFile(passes, out, nullptr, llvm::CodeGenFileType::ObjectFile))
        throw Error("LLVM target cannot emit native object files for " + cfg.triple);
    passes.run(module);
    out.flush();
    if (out.has_error()) throw Error("failed while writing object file '" + path + "'");
}

void writeIR(llvm::Module& module, const std::optional<std::string>& path, bool release = false,
             const std::string& customTriple = "", const std::string& customCpu = "",
             bool baremetal = false, OptReport* report = nullptr, bool sizeOpt = false) {
    if (release) {
        initializeLoomTargets();
        TargetConfig cfg = TargetConfig::resolve(release, customTriple, customCpu, baremetal, sizeOpt);
        std::string error;
        if (auto machine = cfg.createMachine(error)) {
            if (report) {
                report->targetTriple = cfg.triple;
                report->arch = cfg.archName();
                report->osPlatform = cfg.osName();
                report->cpu = cfg.cpu;
                report->features = cfg.features;
            }
            module.setTargetTriple(machine->getTargetTriple());
            module.setDataLayout(machine->createDataLayout());
            optimizeModule(module, *machine, true, sizeOpt);
        }
    }
    if (!path) {
        module.print(llvm::outs(), nullptr);
        return;
    }
    std::error_code ec;
    llvm::raw_fd_ostream out(*path, ec);
    if (ec)
        throw Error("cannot write output file '" + *path + "': " + ec.message());
    module.print(out, nullptr);
    out.flush();
    if (out.has_error())
        throw Error("failed while writing output file '" + *path + "'");
}

std::vector<std::filesystem::path> getSdkIncludeDirs(const char* argv0, const std::string& explicitSdk = "") {
    std::vector<std::filesystem::path> sdkRoots;
    if (!explicitSdk.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(explicitSdk, ec)) {
            sdkRoots.push_back(std::filesystem::canonical(explicitSdk, ec));
        }
    }
    for (const char* envName : {"LOOM_SDK_PATH", "LOOM_SDK"}) {
        if (const char* env = std::getenv(envName)) {
            std::error_code ec;
            if (std::filesystem::is_directory(env, ec)) {
                sdkRoots.push_back(std::filesystem::canonical(env, ec));
            }
        }
    }
    std::filesystem::path exePath = getExecutablePath(argv0);
    if (!exePath.empty()) {
        std::filesystem::path exeDir = exePath.parent_path();
        for (const auto& rel : {
            exeDir.parent_path() / "share" / "loom" / "sdk",
            exeDir.parent_path() / "sdk",
            exeDir / "sdk"
        }) {
            std::error_code ec;
            if (std::filesystem::is_directory(rel, ec)) {
                sdkRoots.push_back(std::filesystem::canonical(rel, ec));
            }
        }
    }
    if (const char* home = std::getenv("HOME")) {
        std::filesystem::path homeSdk = std::filesystem::path(home) / ".loom" / "sdk";
        std::error_code ec;
        if (std::filesystem::is_directory(homeSdk, ec)) {
            sdkRoots.push_back(std::filesystem::canonical(homeSdk, ec));
        }
    }
    for (const auto& sys : {"/usr/local/share/loom/sdk", "/usr/share/loom/sdk", "/opt/loom/sdk"}) {
        std::error_code ec;
        if (std::filesystem::is_directory(sys, ec)) {
            sdkRoots.push_back(std::filesystem::canonical(sys, ec));
        }
    }
    {
        std::filesystem::path cwdSdk = std::filesystem::current_path() / "sdk";
        std::error_code ec;
        if (std::filesystem::is_directory(cwdSdk, ec)) {
            sdkRoots.push_back(std::filesystem::canonical(cwdSdk, ec));
        }
    }

    std::vector<std::filesystem::path> includeDirs;
    std::unordered_set<std::string> seen;
    auto addDir = [&](const std::filesystem::path& p) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            std::filesystem::path can = std::filesystem::canonical(p, ec);
            std::string s = ec ? p.string() : can.string();
            if (seen.insert(s).second) {
                includeDirs.push_back(can);
            }
        }
    };

    for (const auto& root : sdkRoots) {
        addDir(root / "targets" / "rp2040" / "bsp");
        addDir(root / "targets" / "rp2040");
        addDir(root / "std");
        addDir(root);
        std::error_code ec;
        if (std::filesystem::is_directory(root / "targets", ec)) {
            for (const auto& t : std::filesystem::directory_iterator(root / "targets", ec)) {
                if (t.is_directory(ec)) {
                    addDir(t.path() / "bsp");
                    addDir(t.path());
                }
            }
        }
    }
    return includeDirs;
}


} // namespace loom
