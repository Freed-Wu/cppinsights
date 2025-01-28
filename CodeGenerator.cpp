/******************************************************************************
 *
 * C++ Insights, copyright (C) by Andreas Fertig
 * Distributed under an MIT license. See LICENSE for details
 *
 ****************************************************************************/

#include <algorithm>
#include <optional>
#include <vector>

#include "ASTHelpers.h"
#include "ClangCompat.h"
#include "CodeGenerator.h"
#include "DPrint.h"
#include "Insights.h"
#include "InsightsHelpers.h"
#include "InsightsOnce.h"
#include "InsightsStrCat.h"
#include "NumberIterator.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"
//-----------------------------------------------------------------------------

/// \brief Convenience macro to create a \ref LambdaScopeHandler on the stack.
#define LAMBDA_SCOPE_HELPER(type)                                                                                      \
    LambdaScopeHandler lambdaScopeHandler{mLambdaStack, mOutputFormatHelper, LambdaCallerType::type};
//-----------------------------------------------------------------------------

/// \brief The lambda scope helper is only created if cond is true
#define CONDITIONAL_LAMBDA_SCOPE_HELPER(type, cond)                                                                    \
    std::optional<LambdaScopeHandler> lambdaScopeHandler;                                                              \
    if(cond) {                                                                                                         \
        lambdaScopeHandler.emplace(mLambdaStack, mOutputFormatHelper, LambdaCallerType::type);                         \
    }
//-----------------------------------------------------------------------------

namespace ranges = std::ranges;
//-----------------------------------------------------------------------------
namespace clang::insights {

#define BUILD_OPT_AND(name, param) std::function name = [](param t) -> MyOptional<param>
#define BUILD_OPT_AND_O(name, param, ret) std::function name = [](param t) -> MyOptional<ret>

BUILD_OPT_AND(IsPointer, QualType)
{
    if(t->isPointerType()) {
        return t;
    }

    return {};
};

BUILD_OPT_AND(IsPOD, QualType)
{
    if(t.isPODType(GetGlobalAST())) {
        return t;
    }

    return {};
};

template<typename T>
std::function Isa = [](QualType t) -> MyOptional<QualType> {
    if(isa<T>(t.getTypePtrOrNull())) {
        return t;
    }

    return {};
};

BUILD_OPT_AND_O(CanonicalType, const InitListExpr&, QualType)
{
    return t.getType().getCanonicalType();
};

static std::string AccessToStringWithColon(const AccessSpecifier& access)
{
    std::string accessStr{getAccessSpelling(access)};
    if(not accessStr.empty()) {
        accessStr += ": "sv;
    }

    return accessStr;
}
//-----------------------------------------------------------------------------

using namespace asthelpers;

static std::string_view GetCastName(const CastKind castKind, bool constnessChange = false)
{
    if(is{castKind}.any_of(CastKind::CK_BitCast, CastKind::CK_IntegralToPointer, CastKind::CK_PointerToIntegral)) {
        return kwReinterpretCast;
    }

    if((CastKind::CK_NoOp == castKind) and constnessChange) {
        return "const_cast"sv;
    }

    return kwStaticCast;
}
//-----------------------------------------------------------------------------

static std::string_view GetTagDeclTypeName(const TagDecl& decl)
{
    if(decl.isClass()) {
        return kwClassSpace;

    } else if(decl.isUnion()) {
        return kwUnionSpace;

    } else {
        return kwStructSpace;
    }
}
//-----------------------------------------------------------------------------

class ArrayInitCodeGenerator final : public CodeGenerator
{
    const uint64_t mIndex;

public:
    ArrayInitCodeGenerator(OutputFormatHelper& _outputFormatHelper, const uint64_t index)
    : CodeGenerator{_outputFormatHelper}
    , mIndex{index}
    {
    }

    using CodeGenerator::InsertArg;
    void InsertArg(const ArrayInitIndexExpr*) override { mOutputFormatHelper.Append(mIndex); }
};
//-----------------------------------------------------------------------------

/// Handling specialties for decomposition declarations.
///
/// Decompositions declarations have no name. This class stores the made up name and returns it each time the anonymous
/// declaration is asked for a name.
class StructuredBindingsCodeGenerator final : public CodeGenerator
{
    std::string mVarName;

public:
    StructuredBindingsCodeGenerator(OutputFormatHelper& _outputFormatHelper, std::string&& varName)
    : CodeGenerator{_outputFormatHelper}
    , mVarName{std::move(varName)}
    {
    }

    using CodeGenerator::InsertArg;
    void InsertArg(const DeclRefExpr* stmt) override;
    void InsertArg(const BindingDecl* stmt) override;

    /// Inserts the bindings of a decompositions declaration.
    void InsertDecompositionBindings(const DecompositionDecl& decompositionDeclStmt);

protected:
    virtual bool ShowXValueCasts() const override { return true; }
};
//-----------------------------------------------------------------------------

/// Handle using statements which pull functions ore members from a base class into the class.
class UsingCodeGenerator final : public CodeGenerator
{
public:
    UsingCodeGenerator(OutputFormatHelper& _outputFormatHelper)
    : CodeGenerator{_outputFormatHelper}
    {
    }

    using CodeGenerator::InsertArg;
    void InsertArg(const CXXMethodDecl* stmt) override
    {
        mOutputFormatHelper.Append(kwCppCommentStartSpace);

        InsertCXXMethodDecl(stmt, SkipBody::Yes);
    }

    void InsertArg(const FieldDecl* stmt) override
    {
        mOutputFormatHelper.Append(kwCppCommentStartSpace);
        CodeGenerator::InsertArg(stmt);
    }

    // makes no sense to insert the class when applying it to using
    void InsertArg(const CXXRecordDecl*) override {}

    // makes no sense to insert the typedef when applying it to using
    void InsertArg(const TypedefDecl*) override {}

protected:
    bool InsertNamespace() const override { return true; }
};
//-----------------------------------------------------------------------------

/// \brief A special code generator for Lambda init captures which use \c std::move
class LambdaInitCaptureCodeGenerator final : public CodeGenerator
{
public:
    explicit LambdaInitCaptureCodeGenerator(OutputFormatHelper& outputFormatHelper,
                                            LambdaStackType&    lambdaStack,
                                            std::string_view    varName)
    : CodeGenerator{outputFormatHelper, lambdaStack, ProcessingPrimaryTemplate::No}
    , mVarName{varName}
    {
    }

    using CodeGenerator::InsertArg;

    /// Replace every \c VarDecl with the given variable name. This cover init captures which introduce a new name.
    /// However, it means that _all_ VarDecl's will be changed.
    /// TODO: Check if it is really good to replace all VarDecl's
    void InsertArg(const DeclRefExpr* stmt) override
    {
        if(isa<VarDecl>(stmt->getDecl())) {
            mOutputFormatHelper.Append("_"sv, mVarName);

        } else {

            CodeGenerator::InsertArg(stmt);
        }
    }

private:
    std::string_view mVarName;  ///< The name of the variable that needs to be prefixed with _.
};
//-----------------------------------------------------------------------------

class LambdaNameOnlyCodeGenerator final : public CodeGenerator
{
public:
    using CodeGenerator::CodeGenerator;
    using CodeGenerator::InsertArg;

    void InsertArg(const LambdaExpr* stmt) override { mOutputFormatHelper.Append(GetLambdaName(*stmt), "{}"sv); }
};
//-----------------------------------------------------------------------------

CodeGenerator::LambdaScopeHandler::LambdaScopeHandler(LambdaStackType&       stack,
                                                      OutputFormatHelper&    outputFormatHelper,
                                                      const LambdaCallerType lambdaCallerType)
: mStack{stack}
, mHelper{lambdaCallerType, GetBuffer(outputFormatHelper)}
{
    mStack.push(mHelper);
}
//-----------------------------------------------------------------------------

CodeGenerator::LambdaScopeHandler::~LambdaScopeHandler()
{
    if(not mStack.empty()) {
        mStack.pop()->finish();
    }
}
//-----------------------------------------------------------------------------

OutputFormatHelper& CodeGenerator::LambdaScopeHandler::GetBuffer(OutputFormatHelper& outputFormatHelper) const
{
    // Find the most outer element to place the lambda class definition. For example, if we have this:
    // Test( [&]() {} );
    // The lambda's class definition needs to be placed _before_ the CallExpr to Test.
    for(auto& l : mStack) {
        switch(l.callerType()) {
            case LambdaCallerType::CallExpr: [[fallthrough]];
            case LambdaCallerType::VarDecl: [[fallthrough]];
            case LambdaCallerType::ReturnStmt: [[fallthrough]];
            case LambdaCallerType::OperatorCallExpr: [[fallthrough]];
            case LambdaCallerType::MemberCallExpr: [[fallthrough]];
            case LambdaCallerType::BinaryOperator: [[fallthrough]];
            case LambdaCallerType::CXXMethodDecl: return l.buffer();
            default: break;
        }
    }

    return outputFormatHelper;
}
//-----------------------------------------------------------------------------

static std::string_view ArrowOrDot(bool isArrow)
{
    return isArrow ? "->"sv : "."sv;
}
//-----------------------------------------------------------------------------

template<typename T>
static T ValueOrDefault(bool b, T v)
{
    if(b) {
        return v;
    }

    return {};
}
//-----------------------------------------------------------------------------

template<typename T>
static T ValueOr(bool b, T val, T el)
{
    if(b) {
        return val;
    }

    return el;
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXDependentScopeMemberExpr* stmt)
{
    if(not stmt->isImplicitAccess()) {
        InsertArg(stmt->getBase());
    } else {
        InsertNamespace(stmt->getQualifier());
    }

    std::string_view op{ValueOrDefault(not stmt->isImplicitAccess(), ArrowOrDot(stmt->isArrow()))};

    mOutputFormatHelper.Append(op, stmt->getMemberNameInfo().getAsString());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXForRangeStmt* rangeForStmt)
{
    auto&      langOpts{GetLangOpts(*rangeForStmt->getLoopVariable())};
    const bool onlyCpp11{not langOpts.CPlusPlus17};

    auto* rwStmt = const_cast<CXXForRangeStmt*>(rangeForStmt);

    StmtsContainer outerScopeStmts{};

    // C++20 init-statement
    outerScopeStmts.Add(rangeForStmt->getInit());

    // range statement
    outerScopeStmts.Add(rangeForStmt->getRangeStmt());

    if(not onlyCpp11) {
        outerScopeStmts.Add(rangeForStmt->getBeginStmt());
        outerScopeStmts.Add(rangeForStmt->getEndStmt());
    }

    // add the loop variable to the body
    StmtsContainer bodyStmts{};
    bodyStmts.Add(rangeForStmt->getLoopVarStmt());

    // add the body itself, without the CompoundStmt
    bodyStmts.AddBodyStmts(rwStmt->getBody());

    const auto& ctx = rangeForStmt->getLoopVariable()->getASTContext();

    // In case of a range-based for-loop inside an unevaluated template the begin and end statements are not present. In
    // this case just add a nullptr.
    auto* declStmt = [&]() -> DeclStmt* {
        if(onlyCpp11) {
            return mkDeclStmt(rwStmt->getBeginStmt() ? rwStmt->getBeginStmt()->getSingleDecl() : nullptr,
                              rwStmt->getEndStmt() ? rwStmt->getEndStmt()->getSingleDecl() : nullptr);
        }

        return nullptr;
    }();

    auto* innerScope = mkCompoundStmt(bodyStmts, rangeForStmt->getBeginLoc(), rangeForStmt->getEndLoc());

    auto* forStmt = new(ctx) ForStmt(ctx,
                                     declStmt,
                                     rwStmt->getCond(),
                                     rwStmt->getLoopVariable(),
                                     rwStmt->getInc(),
                                     innerScope,
                                     rangeForStmt->getBeginLoc(),
                                     rangeForStmt->getEndLoc(),
                                     rangeForStmt->getEndLoc());

    outerScopeStmts.Add(forStmt);

    auto* outerScope = mkCompoundStmt(outerScopeStmts, rangeForStmt->getBeginLoc(), rangeForStmt->getEndLoc());

    InsertArg(outerScope);

    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertQualifierAndName(const DeclarationName&     declName,
                                           const NestedNameSpecifier* qualifier,
                                           const bool                 hasTemplateKeyword)
{
    mOutputFormatHelper.Append(ScopeHandler::RemoveCurrentScope(GetNestedName(qualifier)),
                               ValueOrDefault(hasTemplateKeyword, kwTemplateSpace),
                               declName.getAsString());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertNamespace(const NestedNameSpecifier* stmt)
{
    mOutputFormatHelper.Append(ScopeHandler::RemoveCurrentScope(GetNestedName(stmt)));
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const UnresolvedLookupExpr* stmt)
{
    InsertQualifierAndNameWithTemplateArgs(stmt->getName(), stmt);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const DependentScopeDeclRefExpr* stmt)
{
    InsertQualifierAndNameWithTemplateArgs(stmt->getDeclName(), stmt);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const VarTemplateDecl* stmt)
{
    const auto* templatedDecl = stmt->getTemplatedDecl();

    // Insert only the primary template here. The specializations are inserted via their instantiated
    // VarTemplateSpecializationDecl which resolved to a VarDecl. It looks like whether the variable has an initializer
    // or not can be used to distinguish between the primary template and one appearing in a templated class.
    RETURN_IF(not templatedDecl->hasInit());

    // VarTemplatedDecl's can have lambdas as initializers. Push a VarDecl on the stack, otherwise the lambda would
    // appear in the middle of template<....> and the variable itself.
    {
        LAMBDA_SCOPE_HELPER(Decltype);  // Needed for P0315Checker
        mLambdaStack.back().setInsertName(true);

        InsertTemplateParameters(*stmt->getTemplateParameters());
    }

    LAMBDA_SCOPE_HELPER(VarDecl);

    InsertArg(templatedDecl);

    for(OnceTrue first{}; const auto* spec : stmt->specializations()) {
        if(TSK_ExplicitSpecialization == spec->getSpecializationKind()) {
            continue;
        }

        if(first) {
            mOutputFormatHelper.AppendNewLine();
        }

        InsertArg(spec);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ConceptDecl* stmt)
{
    LAMBDA_SCOPE_HELPER(Decltype);

    InsertTemplateParameters(*stmt->getTemplateParameters());
    mOutputFormatHelper.Append(kwConceptSpace, stmt->getName(), hlpAssing);

    InsertArg(stmt->getConstraintExpr());
    mOutputFormatHelper.AppendSemiNewLine();
    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ConditionalOperator* stmt)
{
    InsertArg(stmt->getCond());
    mOutputFormatHelper.Append(" ? "sv);
    InsertArg(stmt->getLHS());
    mOutputFormatHelper.Append(" : "sv);
    InsertArg(stmt->getRHS());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const DoStmt* stmt)
{
    mOutputFormatHelper.Append(kwDoSpace);

    WrapInCompoundIfNeeded(stmt->getBody(), AddNewLineAfter::No);

    mOutputFormatHelper.Append(kwWhile);
    WrapInParens([&]() { InsertArg(stmt->getCond()); }, AddSpaceAtTheEnd::No);

    mOutputFormatHelper.AppendSemiNewLine();
    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CaseStmt* stmt)
{
    mOutputFormatHelper.Append(kwCaseSpace);
    InsertArg(stmt->getLHS());

    mOutputFormatHelper.Append(": "sv);
    InsertArg(stmt->getSubStmt());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const BreakStmt* /*stmt*/)
{
    mOutputFormatHelper.Append(kwBreak);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const DefaultStmt* stmt)
{
    mOutputFormatHelper.Append("default: "sv);
    InsertArg(stmt->getSubStmt());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ContinueStmt* /*stmt*/)
{
    mOutputFormatHelper.Append(kwContinue);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const GotoStmt* stmt)
{
    mOutputFormatHelper.Append(kwGotoSpace);
    InsertArg(stmt->getLabel());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const LabelStmt* stmt)
{
    mOutputFormatHelper.AppendNewLine(stmt->getName(), ":"sv);

    if(stmt->getSubStmt()) {
        InsertArg(stmt->getSubStmt());
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const SwitchStmt* stmt)
{
    const bool hasInit{stmt->getInit() or stmt->getConditionVariable()};

    if(hasInit) {
        mOutputFormatHelper.OpenScope();

        InsertIfOrSwitchInitVariables(stmt);
    }

    mOutputFormatHelper.Append(kwSwitch);

    WrapInParens([&]() { InsertArg(stmt->getCond()); }, AddSpaceAtTheEnd::Yes);

    InsertArg(stmt->getBody());

    if(hasInit) {
        mOutputFormatHelper.CloseScope();
    }

    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const WhileStmt* stmt)
{
    auto* rwStmt = const_cast<WhileStmt*>(stmt);
    auto* conditionVar{rwStmt->getConditionVariable()};

    {
        // We need to handle the case that a lambda is used in the init-statement of the for-loop.
        LAMBDA_SCOPE_HELPER(VarDecl);

        if(conditionVar) {
            mOutputFormatHelper.OpenScope();

            InsertArg(conditionVar);
        }

        mOutputFormatHelper.Append(kwWhile);
        WrapInParens([&]() { InsertArg(stmt->getCond()); }, AddSpaceAtTheEnd::Yes);
    }

    if(not conditionVar) {
        WrapInCompoundIfNeeded(stmt->getBody(), AddNewLineAfter::Yes);
    } else {
        const auto&    ctx = GetGlobalAST();
        StmtsContainer bodyStmts{};

        bodyStmts.AddBodyStmts(rwStmt->getBody());
        bodyStmts.AddBodyStmts(Assign(conditionVar, conditionVar->getInit()));

        InsertArg(mkCompoundStmt(bodyStmts, stmt->getBeginLoc(), stmt->getEndLoc()));
    }

    if(conditionVar) {
        mOutputFormatHelper.CloseScope();
    }

    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

/// Get the name of a \c FieldDecl in case this \c FieldDecl is part of a lambda. The name has to be retrieved from the
/// capture fields or can be \c __this.
static std::optional<std::string> GetFieldDeclNameForLambda(const FieldDecl&     fieldDecl,
                                                            const CXXRecordDecl& cxxRecordDecl)
{
    if(cxxRecordDecl.isLambda()) {
        llvm::DenseMap<const ValueDecl*, FieldDecl*> captures{};

        FieldDecl* thisCapture{};

        cxxRecordDecl.getCaptureFields(captures, thisCapture);

        if(&fieldDecl == thisCapture) {
            return std::string{kwInternalThis};
        } else {
            for(const auto& [key, value] : captures) {
                if(&fieldDecl == value) {
                    return GetName(*key);
                }
            }
        }
    }

    return {};
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const SourceLocExpr* stmt)
{
    mOutputFormatHelper.Append(stmt->getBuiltinStr(), "()"sv);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const MemberExpr* stmt)
{
    const auto* base = stmt->getBase();
    const bool  skipBase{[&] {
        if(const auto* implicitCast = dyn_cast_or_null<ImplicitCastExpr>(base)) {
            if(CastKind::CK_UncheckedDerivedToBase == implicitCast->getCastKind()) {
                // if this calls a protected function we cannot cast it to the base, this would not compile
                return isa<CXXThisExpr>(implicitCast->IgnoreImpCasts());
            }
        }

        return false;
    }()};

    if(skipBase) {
        mOutputFormatHelper.Append(kwCCommentStartSpace);
    }

    InsertArg(base);

    const auto* meDecl = stmt->getMemberDecl();
    bool        skipTemplateArgs{false};
    const auto  name = [&]() -> std::string {
        // Handle a special case where we have a lambda static invoke operator. In that case use the appropriate
        // using retType as return type
        if(const auto* m = dyn_cast_or_null<CXXMethodDecl>(meDecl)) {
            if(const auto* rd = m->getParent(); rd and rd->isLambda() and isa<CXXConversionDecl>(m)) {
                skipTemplateArgs = true;

                return StrCat(kwOperatorSpace, GetLambdaName(*rd), "::"sv, BuildRetTypeName(*rd));
            }
        }

        // This is at least the case for lambdas, where members are created by capturing a structured binding. See #181.
        else if(const auto* fd = dyn_cast_or_null<FieldDecl>(meDecl)) {
            if(const auto* cxxRecordDecl = dyn_cast_or_null<CXXRecordDecl>(fd->getParent())) {
                if(const auto& fieldName = GetFieldDeclNameForLambda(*fd, *cxxRecordDecl)) {
                    return fieldName.value();
                }
            }
        }

        // Special case. If this is a CXXConversionDecl it might be:
        // a) a template so we need the template arguments from this type
        // b) in a namespace and need want to preserve that one.
        if(const auto* convDecl = dyn_cast_or_null<CXXConversionDecl>(meDecl)) {
            return StrCat(kwOperatorSpace, GetName(convDecl->getConversionType()));
        }

        return stmt->getMemberNameInfo().getName().getAsString();
    }();

    mOutputFormatHelper.Append(ArrowOrDot(stmt->isArrow()));

    if(skipBase) {
        mOutputFormatHelper.Append(kwSpaceCCommentEndSpace);
    }

    mOutputFormatHelper.Append(name);

    RETURN_IF(skipTemplateArgs);

    if(const auto cxxMethod = dyn_cast_or_null<CXXMethodDecl>(meDecl)) {
        if(const auto* tmplArgs = cxxMethod->getTemplateSpecializationArgs()) {
            OutputFormatHelper ofm{};

            ofm.Append('<');

            bool haveArg{false};
            for(OnceFalse needsComma{}; const auto& arg : tmplArgs->asArray()) {
                if(arg.getKind() == TemplateArgument::Integral) {
                    ofm.AppendComma(needsComma);

                    ofm.Append(arg.getAsIntegral());
                    haveArg = true;
                } else {

                    break;
                }
            }

            if(haveArg) {
                mOutputFormatHelper.Append(ofm, ">"sv);

            } else if(not isa<CXXConversionDecl>(meDecl)) {  // A special case from p0892 a templated conversion
                                                             // operator does not carry the specialization args...
                InsertTemplateArgs(*tmplArgs);
            }
        }
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const UnaryExprOrTypeTraitExpr* stmt)
{
    mOutputFormatHelper.Append(std::string_view{getTraitSpelling(stmt->getKind())});

    if(not stmt->isArgumentType()) {
        const auto* argExpr = stmt->getArgumentExpr();
        const bool  needsParens{not isa<ParenExpr>(argExpr)};

        WrapInParensIfNeeded(needsParens, [&] { InsertArg(argExpr); });

    } else {
        WrapInParens([&] { mOutputFormatHelper.Append(GetName(stmt->getTypeOfArgument())); });
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const IntegerLiteral* stmt)
{
    const auto& type     = stmt->getType();
    const bool  isSigned = type->isSignedIntegerType();

    mOutputFormatHelper.Append(llvm::toString(stmt->getValue(), 10, isSigned));

    InsertSuffix(type);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const FloatingLiteral* stmt)
{
    mOutputFormatHelper.Append(stmt->getValue());
    InsertSuffix(stmt->getType());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXTypeidExpr* stmt)
{
    mOutputFormatHelper.Append(kwTypeId);
    WrapInParens([&]() {
        if(stmt->isTypeOperand()) {
            mOutputFormatHelper.Append(GetName(stmt->getTypeOperand(const_cast<ASTContext&>(GetGlobalAST()))));
        } else {
            InsertArg(stmt->getExprOperand());
        }
    });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const BinaryOperator* stmt)
{
    LAMBDA_SCOPE_HELPER(BinaryOperator);

    BackupAndRestore _{mLastExpr, stmt->getLHS()};

    const bool needLHSParens{isa<BinaryOperator>(stmt->getLHS()->IgnoreImpCasts())};
    WrapInParensIfNeeded(needLHSParens, [&] { InsertArg(stmt->getLHS()); });

    mOutputFormatHelper.Append(" "sv, stmt->getOpcodeStr(), " "sv);

    const bool needRHSParens{isa<BinaryOperator>(stmt->getRHS()->IgnoreImpCasts())};
    WrapInParensIfNeeded(needRHSParens, [&] { InsertArg(stmt->getRHS()); });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CompoundAssignOperator* stmt)
{
    LAMBDA_SCOPE_HELPER(BinaryOperator);

    const bool needLHSParens{isa<BinaryOperator>(stmt->getLHS()->IgnoreImpCasts())};
    WrapInParensIfNeeded(needLHSParens, [&] { InsertArg(stmt->getLHS()); });

    mOutputFormatHelper.Append(hlpAssing);

    // we may need a cast around this back to the src type
    const bool needCast{stmt->getLHS()->getType() != stmt->getComputationLHSType()};
    if(needCast) {
        mOutputFormatHelper.Append(kwStaticCast, "<"sv, GetName(stmt->getLHS()->getType()), ">("sv);
    }

    WrapInParensIfNeeded(needLHSParens, [&] {
        clang::ExprResult res = stmt->getLHS();

        // This cast is not present in the AST. However, if the LHS type is smaller than RHS there is an implicit cast
        // to RHS-type and the result is casted back to LHS-type: static_cast<LHSTy>( static_cast<RHSTy>(LHS) + RHS )
        if(const auto resultingType = GetGlobalCI().getSema().PrepareScalarCast(res, stmt->getComputationLHSType());
           resultingType != CK_NoOp) {
            const QualType castDestType = stmt->getComputationLHSType();
            FormatCast(kwStaticCast, castDestType, stmt->getLHS(), resultingType);
        } else {
            InsertArg(stmt->getLHS());
        }
    });

    mOutputFormatHelper.Append(
        " "sv, BinaryOperator::getOpcodeStr(BinaryOperator::getOpForCompoundAssignment(stmt->getOpcode())), " "sv);

    const bool needRHSParens{isa<BinaryOperator>(stmt->getRHS()->IgnoreImpCasts())};
    WrapInParensIfNeeded(needRHSParens, [&] { InsertArg(stmt->getRHS()); });

    if(needCast) {
        mOutputFormatHelper.Append(")"sv);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXRewrittenBinaryOperator* stmt)
{
    LAMBDA_SCOPE_HELPER(BinaryOperator);

    InsertArg(stmt->getSemanticForm());
}
//-----------------------------------------------------------------------------

static std::string_view GetStorageClassAsString(const StorageClass& sc)
{
    if(SC_None != sc) {
        return VarDecl::getStorageClassSpecifierString(sc);
    }

    return {};
}
//-----------------------------------------------------------------------------

static std::string GetStorageClassAsStringWithSpace(const StorageClass& sc)
{
    std::string ret{GetStorageClassAsString(sc)};

    if(not ret.empty()) {
        ret.append(" "sv);
    }

    return ret;
}
//-----------------------------------------------------------------------------

static std::string GetQualifiers(const VarDecl& vd)
{
    std::string qualifiers{};

    if(vd.isInline() or vd.isInlineSpecified()) {
        qualifiers += kwInlineSpace;
    }

    qualifiers += GetStorageClassAsStringWithSpace(vd.getStorageClass());

    if(vd.isConstexpr()) {
        qualifiers += kwConstExprSpace;
    }

    return qualifiers;
}
//-----------------------------------------------------------------------------

static std::string FormatVarTemplateSpecializationDecl(const Decl* decl, std::string&& defaultName)
{
    std::string name{std::move(defaultName)};

    if(const auto* tvd = dyn_cast_or_null<VarTemplateSpecializationDecl>(decl)) {
        OutputFormatHelper   outputFormatHelper{};
        CodeGeneratorVariant codeGenerator{outputFormatHelper};

        codeGenerator->InsertTemplateArgs(tvd->getTemplateArgs());

        name += outputFormatHelper;
    }

    return name;
}
//-----------------------------------------------------------------------------

/// \brief Find a \c DeclRefExpr belonging to a \c DecompositionDecl
class BindingDeclFinder : public ConstStmtVisitor<BindingDeclFinder>
{
    bool mIsBinding{};

public:
    BindingDeclFinder() = default;

    void VisitDeclRefExpr(const DeclRefExpr* expr)
    {
        if(isa<DecompositionDecl>(expr->getDecl())) {
            mIsBinding = true;
        }
    }

    void VisitStmt(const Stmt* stmt)
    {
        for(const auto* child : stmt->children()) {
            if(child) {
                Visit(child);
            }

            RETURN_IF(mIsBinding);
        }
    }

    bool Find(const Stmt* stmt)
    {
        if(stmt) {
            VisitStmt(stmt);
        }

        return mIsBinding;
    }
};
//-----------------------------------------------------------------------------

/// \brief Find a \c DeclRefExpr belonging to a \c DecompositionDecl
class TemporaryDeclFinder : public StmtVisitor<TemporaryDeclFinder>
{
    CodeGenerator&        codeGenerator;
    bool                  mFound{};
    bool                  mHaveTemporary{};
    Stmt*                 mPrevStmt{};
    std::string           mTempName{};
    std::vector<VarDecl*> mDecls{};

public:
    TemporaryDeclFinder(CodeGenerator& _codeGenerator, const Stmt* stmt, bool inspectReturn = false)
    : codeGenerator{_codeGenerator}
    , mPrevStmt{const_cast<Stmt*>(stmt)}
    {
        RETURN_IF(not GetInsightsOptions().ShowLifetime);

        Visit(mPrevStmt);

        for(auto d : mDecls) {
            codeGenerator.InsertArg(d);
        }

        RETURN_IF(not GetInsightsOptions().UseShow2C or mFound or not inspectReturn);

        if(auto* expr = dyn_cast_or_null<CXXConstructExpr>(stmt)) {
            mTempName = GetTemporaryName(*expr);
#if 0
                auto* dummy = Function("dummy"sv, VoidTy(), {});

                auto* vd = Variable(mTempName, expr->getType(), dummy->getDeclContext());
                dummy->getDeclContext()->addDecl(vd);
                vd->setInit(const_cast<CXXConstructExpr*>(expr));
                vd->setStorageClass(SC_None);
#else
            // XXX hack. Our normal VarDecl is at TU level. It then appears in cxa_start...
            auto& ctx = GetGlobalAST();
            auto* vd  = ImplicitParamDecl::Create(const_cast<ASTContext&>(ctx),
                                                 ctx.getTranslationUnitDecl(),
                                                  {},
                                                 &ctx.Idents.get(mTempName),
                                                 expr->getType(),
                                                 ImplicitParamKind::Other);

#endif

            mFound = true;
            codeGenerator.InsertArg(vd);

        } else if(auto* expr = dyn_cast_or_null<InitListExpr>(stmt)) {
            mTempName = GetTemporaryName(*expr);
            auto* vd  = Variable(mTempName, expr->getType());
            vd->setInit(const_cast<InitListExpr*>(expr));
            mFound = true;
            codeGenerator.InsertArg(vd);
        }
    }

    ~TemporaryDeclFinder()
    {
        if(mHaveTemporary) {
            codeGenerator.EndLifetimeScope();
        }
    }

    bool Found() const { return mFound; }

    std::string Name() const { return mTempName; }

    void VisitCXXTemporaryObjectExpr(CXXTemporaryObjectExpr* expr)
    {
        mTempName = GetName(*expr);
        mFound    = true;

        auto* vd = Variable(mTempName, expr->getType());

        // In the Cfront case the contents of the expression go after the generated constructor. In the lifetime
        // _only_ case go with the variable.
        if(not GetInsightsOptions().UseShow2C) {
            // Since we insert the statement below we must clone this expression otherwise we look at a recursion.
            auto* ctorConstructExpr = CXXConstructExpr::Create(GetGlobalAST(),
                                                               expr->getType(),
                                                               expr->getBeginLoc(),
                                                               expr->getConstructor(),
                                                               expr->isElidable(),
                                                               {expr->getArgs(), expr->getNumArgs()},
                                                               expr->hadMultipleCandidates(),
                                                               expr->isListInitialization(),
                                                               expr->isStdInitListInitialization(),
                                                               expr->requiresZeroInitialization(),
                                                               expr->getConstructionKind(),
                                                               expr->getParenOrBraceRange());

            vd->setInit(ctorConstructExpr);

            auto* newValue = mkDeclRefExpr(vd);
            ReplaceNode(mPrevStmt, expr, newValue);
        }

        mDecls.push_back(vd);
    }

#if 0
    void VisitCXXBindTemporaryExpr(CXXBindTemporaryExpr* stmt)
    {
        auto& ctx = GetGlobalAST();

        const auto name = MakeLineColumnName(ctx.getSourceManager(), stmt->getBeginLoc(), "temp"sv);
        mFound          = true;

        auto* vd = Variable(name, stmt->getType());
        codeGenerator.InsertArg(vd);

        StmtVisitor<TemporaryDeclFinder>::VisitCXXBindTemporaryExpr(stmt);
    }
#endif

    void VisitMaterializeTemporaryExpr(MaterializeTemporaryExpr* stmt)
    {
        StmtVisitor<TemporaryDeclFinder>::VisitMaterializeTemporaryExpr(stmt);

        RETURN_IF(mFound);

        auto* vd = Variable(GetTemporaryName(*stmt), stmt->getType());
        vd->setInit(stmt->getSubExpr());

        auto* newValue = mkDeclRefExpr(vd);
        ReplaceNode(mPrevStmt, stmt, newValue);

        if(SD_FullExpression == stmt->getStorageDuration() and not mHaveTemporary) {
            codeGenerator.StartLifetimeScope();
            mHaveTemporary = true;
        } else if(const auto* extending = stmt->getExtendingDecl()) {
            codeGenerator.LifetimeAddExtended(vd, extending);
        }

        mDecls.push_back(vd);
    }

    void VisitStmt(Stmt* stmt)
    {
        auto* tmp = mPrevStmt;
        mPrevStmt = stmt;

        for(auto* child : stmt->children()) {
            Visit(child);
        }

        mPrevStmt = tmp;
    }

    void Visit(Stmt* stmt)
    {
        if(stmt) {
            StmtVisitor<TemporaryDeclFinder>::Visit(stmt);
        }
    }
};
//-----------------------------------------------------------------------------

/*constinit*/ static SmallVector<std::pair<std::pair<const CXXRecordDecl*, const CXXRecordDecl*>, VarDecl*>, 10>
                                            gVtables{};
/*constinit*/ static SmallVector<Expr*, 10> globalVarCtors{};
/*constinit*/ static SmallVector<Expr*, 10> globalVarDtors{};
//-----------------------------------------------------------------------------

int GetGlobalVtablePos(const CXXRecordDecl* record, const CXXRecordDecl* recordB)
{
    auto iter = std::ranges::find_if(
        gVtables, [&](const auto& e) { return (e.first.first == record) and (e.first.second == recordB); });

    if(iter == gVtables.end()) {
        iter = std::ranges::find_if(gVtables, [&](const auto& e) { return e.first.first == record; });
    }

    return std::distance(gVtables.begin(), iter);
}
//-----------------------------------------------------------------------------

void PushVtableEntry(const CXXRecordDecl* record, const CXXRecordDecl* recordB, VarDecl* decl)
{
    gVtables.push_back({{record, recordB}, decl});
}
//-----------------------------------------------------------------------------

static void PushGlobalVariable(const Expr* callExpr)
{
    globalVarCtors.push_back(const_cast<Expr*>(callExpr));
}
//-----------------------------------------------------------------------------

static void PushGlobalVariableDtor(const Expr* callExpr)
{
    globalVarDtors.push_back(const_cast<Expr*>(callExpr));
}
//-----------------------------------------------------------------------------

std::string EmitGlobalVariableCtors()
{
    StmtsContainer bodyStmts{};

    for(auto& e : globalVarCtors) {
        bodyStmts.AddBodyStmts(e);
    }

    auto* cxaStartFun = Function(cxaStart, VoidTy(), {});
    cxaStartFun->setBody(mkCompoundStmt(bodyStmts));

    OutputFormatHelper ofm{};
    ofm.AppendNewLine();
    ofm.AppendNewLine();
    CodeGeneratorVariant cg{ofm};

    if(gVtables.size()) {
        SmallVector<Expr*, 16> mInitExprs{};

        for(auto& e : gVtables) {
            cg->InsertArg(e.second);
            mInitExprs.push_back(mkDeclRefExpr(e.second));
        }

        ofm.AppendNewLine();

        // struct __mptr *__ptbl_vec__c___src_C_[]
        auto* vtable = CfrontCodeGenerator::VtableData().VtblArrayVar(mInitExprs.size());
        vtable->setInit(InitList(mInitExprs, vtable->getType()));

        cg->InsertArg(vtable);

        ofm.AppendNewLine();
    }

    cg->InsertArg(cxaStartFun);

    StmtsContainer bodyStmtsDtors{};

    for(auto& e : globalVarDtors) {
        bodyStmtsDtors.AddBodyStmts(e);
    }

    auto* cxaAtExitFun = Function(cxaAtExit, VoidTy(), {});
    cxaAtExitFun->setBody(mkCompoundStmt(bodyStmtsDtors));

    ofm.AppendNewLine();
    cg->InsertArg(cxaAtExitFun);

    return ofm.GetString();
}
//-----------------------------------------------------------------------------

void CodeGenerator::LifetimeAddExtended(const VarDecl* vd, const ValueDecl* extending)
{
    mLifeTimeTracker.AddExtended(vd, extending);
}
//-----------------------------------------------------------------------------

void CodeGenerator::StartLifetimeScope()
{
    mLifeTimeTracker.StartScope(false);
}
//-----------------------------------------------------------------------------

void CodeGenerator::EndLifetimeScope()
{
    mSkipSemi = mLifeTimeTracker.EndScope(mOutputFormatHelper, false) or mSkipSemi;
}
//-----------------------------------------------------------------------------

// In a primary template we can see a ParenListExpr with a PackExpansionExpr. With the equal sign we need a type.
static bool IsPrimaryTemplatePackExpansionExpr(const ParenListExpr* stmt)
{
    return stmt and stmt->getNumExprs() and isa_and_nonnull<PackExpansionExpr>(stmt->getExpr(0)) and
           stmt->getType().isNull();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const LinkageSpecDecl* stmt)
{
    mOutputFormatHelper.Append("extern \"", (LinkageSpecLanguageIDs::C == stmt->getLanguage()) ? "C"sv : "C++"sv, "\"");
    mOutputFormatHelper.OpenScope();

    for(const auto* decl : stmt->decls()) {
        InsertArg(decl);
    }

    mOutputFormatHelper.CloseScope();
    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertTemplateArgsObjectParam(const TemplateParamObjectDecl& param)
{
    PrintingPolicy pp{GetGlobalAST().getLangOpts()};
    pp.adjustForCPlusPlus();

    if(auto varName = GetName(param); not mSeenDecls.contains(varName)) {
        std::string                init{};
        ::llvm::raw_string_ostream stream{init};
        param.printAsInit(stream, pp);

        // https://eel.is/c++draft/temp.param#8 says the variable is `static const`. However, to make the
        // compiler accept the generated code the storage object must be constexpr.
        // The initialization itself is on the lowest level, int's, floating point or nested structs with them. For
        // classes this could fail a all fields even the hidden ones are observed. However, for NTTPs the rule is that
        // only structs/classes with _only_ public data members are accepted.
        mOutputFormatHelper.AppendSemiNewLine(
            "static constexpr ", GetName(param.getType().getUnqualifiedType()), " ", varName, init);
        mSeenDecls[varName] = true;
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertTemplateArgsObjectParam(const ArrayRef<TemplateArgument>& array)
{
    for(const auto& arg : array) {
        if(TemplateArgument::Declaration != arg.getKind()) {
            continue;
        } else if(const auto decl = dyn_cast_or_null<TemplateParamObjectDecl>(arg.getAsDecl())) {
            InsertTemplateArgsObjectParam(*decl);
        }
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertTemplateSpecializationHeader(const Decl& decl)
{
    if(const auto* fd = dyn_cast_or_null<FunctionDecl>(&decl)) {
        if(const auto* specArgs = fd->getTemplateSpecializationArgs()) {
            InsertTemplateArgsObjectParam(specArgs->asArray());
        }
    } else if(const auto* vd = dyn_cast_or_null<VarTemplateSpecializationDecl>(&decl)) {
        InsertTemplateArgsObjectParam(vd->getTemplateArgs().asArray());
    } else if(const auto* clsTemplateSpe = dyn_cast_or_null<ClassTemplateSpecializationDecl>(&decl)) {
        InsertTemplateArgsObjectParam(clsTemplateSpe->getTemplateArgs().asArray());
    }

    mOutputFormatHelper.AppendNewLine(kwTemplate, "<>"sv);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const VarDecl* stmt)
{
    if(auto* init = stmt->getInit();
       GetInsightsOptions().UseShow2C and IsReferenceType(stmt) and
       (not(isa<CallExpr>(init) or
            (isa<ExprWithCleanups>(init) and
             (isa<CallExpr>(dyn_cast_or_null<ExprWithCleanups>(init)->getSubExpr()) or
              isa<MaterializeTemporaryExpr>(dyn_cast_or_null<ExprWithCleanups>(init)->getSubExpr())))))) {
        return;
    }

    mLifeTimeTracker.Add(stmt);

    // If this is part of a DecompositionDecl then ignore this VarDecl as we already have seen and inserted it. This
    // happens in StructuredBindingsHandler3Test.cpp
    if(BindingDeclFinder isBindingDecl{}; isBindingDecl.Find(stmt->getInit())) {
        return;
    }

    LAMBDA_SCOPE_HELPER(VarDecl);
    UpdateCurrentPos(mCurrentVarDeclPos);

    TemporaryDeclFinder temporaryFinder{*this, stmt->getInit()};

    if(InsertComma()) {
        mOutputFormatHelper.Append(',');
    }

    // If we are looking at a static member variable of a class template which is defined out-of-line we need to
    // protect the resulting instantiations.
    const bool needsGuard = stmt->isOutOfLine() and isTemplateInstantiation(stmt->getTemplateSpecializationKind());

    // We are looking at the primary definition of a out-of-line member variable of a class template. We need to add
    // the template head.
    if(stmt->isOutOfLine()) {
        if(const auto* recordDecl = dyn_cast_or_null<CXXRecordDecl>(stmt->getDeclContext())) {
            if(const auto* classTmpl = recordDecl->getDescribedClassTemplate()) {
                InsertTemplateParameters(*classTmpl->getTemplateParameters());
            }
        }
    }

    if(isa<VarTemplateSpecializationDecl>(stmt)) {
        InsertTemplateSpecializationHeader(*stmt);
    } else if(needsGuard) {
        mOutputFormatHelper.InsertIfDefTemplateGuard();
    }

    InsertAttributes(stmt->attrs());
    InsertConceptConstraint(stmt);

    if(IsTrivialStaticClassVarDecl(*stmt)) {
        HandleLocalStaticNonTrivialClass(stmt);

    } else {
        if(InsertVarDecl(stmt)) {
            const auto desugaredType = GetType(GetDesugarType(stmt->getType()));

            const bool isMemberPointer{isa<MemberPointerType>(desugaredType.getTypePtrOrNull())};
            if(desugaredType->isFunctionPointerType() or isMemberPointer) {
                const auto lineNo    = GetSM(*stmt).getSpellingLineNumber(stmt->getSourceRange().getBegin());
                const auto ptrPrefix = isMemberPointer ? memberVariablePointerPrefix : functionPointerPrefix;
                const auto funcPtrName{StrCat(ptrPrefix, lineNo)};

                mOutputFormatHelper.AppendSemiNewLine(kwUsingSpace, funcPtrName, hlpAssing, GetName(desugaredType));
                mOutputFormatHelper.Append(GetQualifiers(*stmt), funcPtrName, " "sv, GetName(*stmt));

            } else {
                mOutputFormatHelper.Append(GetQualifiers(*stmt));

                const auto scope = [&] {
                    if(const auto* ctx = stmt->getDeclContext(); stmt->getLexicalDeclContext() != ctx) {
                        OutputFormatHelper scopeOfm{};
                        scopeOfm.Append(GetDeclContext(ctx, WithTemplateParameters::Yes));

                        return ScopeHandler::RemoveCurrentScope(scopeOfm.GetString());
                    }

                    return std::string{};
                }();

                const auto varName = FormatVarTemplateSpecializationDecl(stmt, StrCat(scope, GetName(*stmt)));

                // TODO: to keep the special handling for lambdas, do this only for template specializations
                mOutputFormatHelper.Append(GetTypeNameAsParameter(GetType(stmt->getType()), varName));
            }
        } else {
            const std::string_view pointer = [&]() {
                if(SkipSpaceAfterVarDecl()) {
                    return ""sv;
                }

                if(stmt->getType()->isAnyPointerType()) {
                    return " *"sv;
                }
                return " "sv;
            }();

            mOutputFormatHelper.Append(pointer, GetName(*stmt));
        }

        if(const auto* init = stmt->getInit()) {
            if(const auto* ctorExpr = dyn_cast_or_null<CXXConstructExpr>(init);
               GetInsightsOptions().UseShow2C and ctorExpr) {

                // https://stackoverflow.com/questions/45471470/how-to-generate-code-for-initializing-global-variables-with-non-const-values-in
                // https://llvm.org/docs/LangRef.html#the-llvm-global-ctors-global-variable
                // https://stackoverflow.com/questions/72971935/why-does-clang-do-thread-safe-init-for-some-globals-but-not-others
                // __cxx_global_var_init
                // https://discourse.llvm.org/t/static-constructors-cxx-global-var-initn-vs-global-sub-i-xxx/39442
                //                if(stmt->needsDestruction(stmt->getASTContext())) {
                //
                // at_exit
                // https://clang.llvm.org/doxygen/CGDeclCXX_8cpp_source.html
                // __cxa_atexit

                //                if(not ctorExpr->getConstructor()->isTrivial()) {
                if(stmt->hasGlobalStorage()) {
                    // push to __cxx_global_var_init
                    auto* callExpr = CallConstructor(
                        stmt->getType(), stmt, ArgsToExprVector(ctorExpr), DoCast::No, AsReference::Yes);

                    PushGlobalVariable(callExpr);

                    PushGlobalVariableDtor(CallDestructor(stmt));

                } else {
                    mOutputFormatHelper.AppendSemiNewLine();
                    InsertArg(init);
                }
                //                }

            } else {
                mProcessingVarDecl = false;
                BackupAndRestore _{mProcessingVarDecl, true};

                if(MyOptional<const InitListExpr*> initList{dyn_cast_or_null<InitListExpr>(init)};
                   GetInsightsOptions().UseShow2C and
                   initList.and_then(CanonicalType).and_then(Isa<RecordType>).and_not(IsPointer).and_then(IsPOD) and
                   not isa<ArrayType>(stmt->getType())) {
                    auto* callMemset = Call("memset"sv, {Ref(stmt), Int32(0), Sizeof(stmt->getType())});

                    EnableGlobalInsert(GlobalInserts::FuncMemset);

                    if(stmt->hasGlobalStorage()) {
                        PushGlobalVariable(callMemset);

                    } else {
                        mOutputFormatHelper.AppendSemiNewLine();
                        InsertArg(callMemset);
                    }

                } else if(not(GetInsightsOptions().UseShow2C and
                              initList.and_then(CanonicalType)
                                  .and_then(Isa<RecordType>)
                                  .and_not(IsPointer)
                                  .and_not(IsPOD) and
                              (0 == initList.value()->getNumInits()))) {

                    // Skip the init statement in case we have a class type with a trivial default-constructor which
                    // is used for this initialization.
                    if(not(ctorExpr and ctorExpr->getConstructor()->isDefaultConstructor() and
                           ctorExpr->getConstructor()->getParent()->hasTrivialDefaultConstructor())) {

                        const bool isPrimaryTemplatePackExpansionExpr{
                            IsPrimaryTemplatePackExpansionExpr(dyn_cast_or_null<ParenListExpr>(init))};

                        if(not isa<CXXParenListInitExpr>(init) and not isPrimaryTemplatePackExpansionExpr) {
                            mOutputFormatHelper.Append(hlpAssing);
                        }

                        WrapInParensIfNeeded(isPrimaryTemplatePackExpansionExpr, [&] {
                            if(GetInsightsOptions().ShowLifetime and init->isXValue() and
                               stmt->getType()->isRValueReferenceType()) {

                                if(GetInsightsOptions().UseShow2C) {
                                    mOutputFormatHelper.Append("&");
                                }

                                InsertArg(StaticCast(stmt->getType(), init, false));

                            } else {
                                InsertArg(init);
                            }
                        });
                    }
                }
            }
        }

        if(stmt->isNRVOVariable()) {
            mOutputFormatHelper.Append(" /* NRVO variable */"sv);
        }

        if(InsertSemi()) {
            mOutputFormatHelper.AppendSemiNewLine();
        }

        // Insert the bindings of a DecompositionDecl if this VarDecl is a DecompositionDecl.
        if(const auto* decompDecl = dyn_cast_or_null<DecompositionDecl>(stmt)) {
            StructuredBindingsCodeGenerator codeGenerator{mOutputFormatHelper, GetName(*stmt)};

            codeGenerator.InsertDecompositionBindings(*decompDecl);
        }
    }

    if(needsGuard) {
        mOutputFormatHelper.InsertEndIfTemplateGuard();
    }
}
//-----------------------------------------------------------------------------

bool CodeGenerator::InsertLambdaStaticInvoker(const CXXMethodDecl* cxxMethodDecl)
{
    if(not(cxxMethodDecl and cxxMethodDecl->isLambdaStaticInvoker())) {
        return false;
    }

    // A special case for a lambda with a static invoker. The standard says, that in such a case invoking the call
    // operator gives the same result as invoking the function pointer (see [expr.prim.lambda.closure] p9). When it
    // comes to block local statics having a body for both functions reveals a difference. This special code
    // generates a forwarding call from the call operator to the static invoker. However, the compiler does better
    // here. As this way we end up with copies of the parameters which is hard to avoid.

    mOutputFormatHelper.AppendNewLine();
    mOutputFormatHelper.OpenScope();

    if(not cxxMethodDecl->getReturnType()->isVoidType()) {
        mOutputFormatHelper.Append(kwReturn, " "sv);
    }

    mOutputFormatHelper.Append(GetName(*cxxMethodDecl->getParent()), "{}.operator()"sv);

    if(cxxMethodDecl->isFunctionTemplateSpecialization()) {
        InsertTemplateArgs(*dyn_cast_or_null<FunctionDecl>(cxxMethodDecl));
    }

    if(cxxMethodDecl->isTemplated()) {
        if(cxxMethodDecl->getDescribedTemplate()) {
            InsertTemplateParameters(*cxxMethodDecl->getDescribedTemplate()->getTemplateParameters(),
                                     TemplateParamsOnly::Yes);
        }
        /*else if(decl.isFunctionTemplateSpecialization()) {
            InsertTemplateSpecializationHeader();
        }*/
    }

    WrapInParens([&] {
        mOutputFormatHelper.AppendParameterList(cxxMethodDecl->parameters(),
                                                OutputFormatHelper::NameOnly::Yes,
                                                OutputFormatHelper::GenMissingParamName::Yes);
    });

    mOutputFormatHelper.AppendSemiNewLine();
    mOutputFormatHelper.CloseScope(OutputFormatHelper::NoNewLineBefore::Yes);
    mOutputFormatHelper.AppendNewLine();

    return true;
}
//-----------------------------------------------------------------------------

/// \brief Inserts the instantiation point of a template.
//
// This reveals at which place the template is first used.
void CodeGenerator::InsertInstantiationPoint(const SourceManager&  sm,
                                             const SourceLocation& instLoc,
                                             std::string_view      text)
{
    const auto  lineNo = sm.getSpellingLineNumber(instLoc);
    const auto& fileId = sm.getFileID(instLoc);
    if(const auto file = sm.getFileEntryRefForID(fileId)) {
        const auto fileWithDirName = file->getName();
        const auto fileName        = llvm::sys::path::filename(fileWithDirName);

        if(text.empty()) {
            text = "First instantiated from: "sv;
        }

        mOutputFormatHelper.AppendCommentNewLine(text, fileName, ":"sv, lineNo);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertTemplateGuardBegin(const FunctionDecl* stmt)
{
    if(stmt->isTemplateInstantiation() and stmt->isFunctionTemplateSpecialization()) {
        InsertInstantiationPoint(GetSM(*stmt), stmt->getPointOfInstantiation());
        mOutputFormatHelper.InsertIfDefTemplateGuard();
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertTemplateGuardEnd(const FunctionDecl* stmt)
{
    if(stmt->isTemplateInstantiation() and stmt->isFunctionTemplateSpecialization()) {
        mOutputFormatHelper.InsertEndIfTemplateGuard();
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CoroutineBodyStmt* stmt)
{
    InsertArg(stmt->getBody());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const DependentCoawaitExpr* stmt)
{
    mOutputFormatHelper.Append(kwCoAwaitSpace);

    InsertArg(stmt->getOperand());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CoroutineSuspendExpr* stmt)
{
    //	co_await or co_yield
    if(isa<CoyieldExpr>(stmt)) {
        mOutputFormatHelper.Append(kwCoYieldSpace);
    } else {
        mOutputFormatHelper.Append(kwCoAwaitSpace);
    }

    // peal of __promise.yield_value
    if(const auto* matTemp = dyn_cast_or_null<MaterializeTemporaryExpr>(stmt->getCommonExpr())) {
        const auto* temporary = matTemp->getSubExpr();

        if(const auto* memExpr = dyn_cast_or_null<CXXMemberCallExpr>(temporary)) {
            ForEachArg(memExpr->arguments(), [&](const auto& arg) { InsertArg(arg); });

            // Seems to be the path for a co_await expr
        } else {
            InsertArg(temporary);
        }
    } else if(const auto* unaryexpr = dyn_cast_or_null<UnaryOperator>(stmt->getOperand())) {
        if(const auto* callExpr = dyn_cast_or_null<CallExpr>(unaryexpr->getSubExpr())) {
            InsertArg(callExpr->getArg(0));
        }
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CoreturnStmt* stmt)
{
    mOutputFormatHelper.Append(kwCoReturnSpace);
    InsertArg(stmt->getOperand());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertMethodBody(const FunctionDecl* stmt, const size_t posBeforeFunc)
{
    auto IsPrimaryTemplate = [&] {
        // For now, don't transform the primary template of a coroutine
        if(const auto* cxxMethod = dyn_cast_or_null<CXXMethodDecl>(stmt)) {
            if(const auto* tmpl = cxxMethod->getParent()->getDescribedClassTemplate();
               tmpl and not isa<ClassTemplateSpecializationDecl>(cxxMethod->getParent())) {
                return true;
            }
        }

        return (FunctionDecl::TK_FunctionTemplate == stmt->getTemplatedKind()) or
               (ProcessingPrimaryTemplate::Yes == mProcessingPrimaryTemplate);
    };

    if(stmt->doesThisDeclarationHaveABody()) {
        mOutputFormatHelper.AppendNewLine();

        // If this function has a CoroutineBodyStmt as direct descend and coroutine transformation is enabled use
        // the \c CoroutinesCodeGenerator, otherwise insert the body as usual.
        if(const auto* corBody = dyn_cast_or_null<CoroutineBodyStmt>(stmt->getBody());
           (nullptr != corBody) and not IsPrimaryTemplate() and GetInsightsOptions().ShowCoroutineTransformation) {

            CoroutinesCodeGenerator codeGenerator{mOutputFormatHelper, posBeforeFunc};
            codeGenerator.InsertCoroutine(*stmt, corBody);
        } else {
            const auto exSpec = stmt->getExceptionSpecType();
            const bool showNoexcept =
                GetInsightsOptions().UseShowNoexcept and is{exSpec}.any_of(EST_BasicNoexcept, EST_NoexceptTrue);

            // handle C++ [basic.start.main] §5: main can have no return statement
            if(stmt->hasImplicitReturnZero()) {
                mRequiresImplicitReturnZero = ranges::none_of(dyn_cast<CompoundStmt>(stmt->getBody())->body(),
                                                              [](const Stmt* e) { return isa<ReturnStmt>(e); });
            }

            const auto* body = stmt->getBody();

            if(showNoexcept) {
                EnableGlobalInsert(GlobalInserts::HeaderException);

                body = mkCompoundStmt(Try(body, Catch(Call("std::terminate"sv, {}))));
            }

            if(GetInsightsOptions().ShowLifetime) {
                for(const auto* param : stmt->parameters()) {
                    auto       paramType = param->getType();
                    const bool isPassByValue{not paramType->isPointerType() and not paramType->isReferenceType()};
                    if(const auto* rd = paramType->getAsRecordDecl(); rd and isPassByValue) {
                        mLifeTimeTracker.Add(param);
                    }
                }
            }

            InsertArg(body);
        }

        mOutputFormatHelper.AppendNewLine();
    } else {
        mOutputFormatHelper.AppendSemiNewLine();
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const FunctionDecl* stmt)
{
    {
        LAMBDA_SCOPE_HELPER(Decltype);  // Needed for P0315Checker

        // Special handling for C++20's P0315 (lambda in unevaluated context). See p0315_2Test.cpp
        // We have to look for the lambda expression in the decltype.
        P0315Visitor dt{*this};
        dt.TraverseType(stmt->getReturnType());

        // The arguments can contain a lambda as well
        for(const auto& param : stmt->parameters()) {
            P0315Visitor dt{*this};
            dt.TraverseType(param->getType());
        }
    }

    if(const auto* deductionGuide = dyn_cast_or_null<CXXDeductionGuideDecl>(stmt)) {
        InsertArg(deductionGuide);
    } else if(const auto* ctor = dyn_cast_or_null<CXXConstructorDecl>(stmt)) {
        InsertArg(ctor);
    } else {
        // skip a case at least in lambdas with a templated conversion operator which is not used and has auto
        // return type. This is hard to build with using.
        RETURN_IF(isa<CXXConversionDecl>(stmt) and not stmt->hasBody());

        const auto posBeforeFunc = mOutputFormatHelper.CurrentPos();

        InsertTemplateGuardBegin(stmt);
        InsertFunctionNameWithReturnType(*stmt);

        if(not InsertLambdaStaticInvoker(dyn_cast_or_null<CXXMethodDecl>(stmt))) {
            InsertMethodBody(stmt, posBeforeFunc);
        }

        InsertTemplateGuardEnd(stmt);
    }
}
//-----------------------------------------------------------------------------

static std::string GetTypeConstraintAsString(const TypeConstraint* typeConstraint)
{
    if(typeConstraint) {
        StringStream sstream{};
        sstream.Print(*typeConstraint);

        return sstream.str();
    }

    return {};
}
//-----------------------------------------------------------------------------

static std::string_view Ellipsis(bool b)
{
    return ValueOrDefault(b, kwElipsis);
}
//-----------------------------------------------------------------------------

static std::string_view EllipsisSpace(bool b)
{
    return ValueOrDefault(b, kwElipsisSpace);
}
//-----------------------------------------------------------------------------

/// \brief Evaluates a potential NTTP as a constant expression.
///
/// Used for C++20's struct/class as NTTP.
static std::optional<std::pair<QualType, APValue>> EvaluateNTTPAsConstantExpr(const Expr* expr)
{
    expr = expr->IgnoreParenImpCasts();

    // The marker when it is a C++20 class as NTTP seems to be CXXFunctionalCastExpr
    if(Expr::EvalResult evalResult{};
       isa<CXXFunctionalCastExpr>(expr) and
       expr->EvaluateAsConstantExpr(evalResult, GetGlobalAST(), ConstantExprKind::Normal)) {
        return std::pair<QualType, APValue>{expr->getType(), evalResult.Val};
    }

    return {};
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertTemplateParameters(const TemplateParameterList& list,
                                             const TemplateParamsOnly     templateParamsOnly)
{
    const bool full{TemplateParamsOnly::No == templateParamsOnly};

    if(full) {
        for(const auto* param : list) {
            if(const auto* nonTmplParam = dyn_cast_or_null<NonTypeTemplateParmDecl>(param);
               nonTmplParam and nonTmplParam->hasDefaultArgument()) {
                if(auto val =
                       EvaluateNTTPAsConstantExpr(nonTmplParam->getDefaultArgument().getArgument().getAsExpr())) {
                    auto* init = GetGlobalAST().getTemplateParamObjectDecl(val->first, val->second);

                    InsertTemplateArgsObjectParam(*init);
                }
            }
        }

        mOutputFormatHelper.Append(kwTemplate);
    }

    mOutputFormatHelper.Append("<"sv);

    for(OnceFalse needsComma{}; const auto* param : list) {
        mOutputFormatHelper.AppendComma(needsComma);

        const auto& typeName = GetName(*param);

        if(const auto* tt = dyn_cast_or_null<TemplateTypeParmDecl>(param)) {
            if(full) {
                if(tt->wasDeclaredWithTypename()) {
                    mOutputFormatHelper.Append(kwTypeNameSpace);
                } else if(not tt->hasTypeConstraint()) {
                    mOutputFormatHelper.Append(kwClassSpace);
                }

                mOutputFormatHelper.Append(EllipsisSpace(tt->isParameterPack()));
            }

            if(0 == typeName.size() or tt->isImplicit() /* fixes class container:auto*/) {
                AppendTemplateTypeParamName(mOutputFormatHelper, tt, not full);

            } else {
                if(auto typeConstraint = GetTypeConstraintAsString(tt->getTypeConstraint());
                   not typeConstraint.empty()) {
                    mOutputFormatHelper.Append(std::move(typeConstraint), " "sv);
                }

                mOutputFormatHelper.Append(typeName);
            }

            mOutputFormatHelper.Append(EllipsisSpace(not full and tt->isParameterPack()));

            if(tt->hasDefaultArgument() and not tt->defaultArgumentWasInherited()) {
                const auto& defaultArg = tt->getDefaultArgument();

                if(const auto decltypeType = dyn_cast_or_null<DecltypeType>(defaultArg.getArgument().getAsType())) {
                    mOutputFormatHelper.Append(hlpAssing);

                    InsertArg(decltypeType->getUnderlyingExpr());

                } else {
                    mOutputFormatHelper.Append(hlpAssing);
                    InsertTemplateArg(defaultArg.getArgument());
                }
            }

        } else if(const auto* nonTmplParam = dyn_cast_or_null<NonTypeTemplateParmDecl>(param)) {
            if(full) {
                if(const auto nttpType = nonTmplParam->getType();
                   nttpType->isFunctionPointerType() or nttpType->isMemberFunctionPointerType()) {
                    mOutputFormatHelper.Append(GetTypeNameAsParameter(nttpType, typeName));

                } else {
                    mOutputFormatHelper.Append(
                        GetName(nttpType), " "sv, Ellipsis(nonTmplParam->isParameterPack()), typeName);
                }

                if(nonTmplParam->hasDefaultArgument()) {
                    mOutputFormatHelper.Append(hlpAssing);
                    InsertTemplateArg(nonTmplParam->getDefaultArgument().getArgument());
                }
            } else {
                mOutputFormatHelper.Append(typeName, EllipsisSpace(nonTmplParam->isParameterPack()));
            }
        } else if(const auto* tmplTmplParam = dyn_cast_or_null<TemplateTemplateParmDecl>(param)) {
            auto pack{ValueOr(tmplTmplParam->isParameterPack(), kwElipsisSpace, " "sv)};

            mOutputFormatHelper.Append(kwTemplateSpace, "<typename> typename"sv, pack, typeName);

            if(tmplTmplParam->hasDefaultArgument()) {
                mOutputFormatHelper.Append(hlpAssing);
                InsertTemplateArg(tmplTmplParam->getDefaultArgument().getArgument());
            }
        }
    }

    mOutputFormatHelper.Append(">"sv);

    if(full) {
        mOutputFormatHelper.AppendNewLine();
        InsertConceptConstraint(list);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ClassTemplateDecl* stmt)
{
    {
        LAMBDA_SCOPE_HELPER(Decltype);  // Needed for P0315Checker
        mLambdaStack.back().setInsertName(true);

        InsertTemplateParameters(*stmt->getTemplateParameters());
    }

    InsertArg(stmt->getTemplatedDecl());

    SmallVector<const ClassTemplateSpecializationDecl*, 10> specializations{};

    // XXX C++23: replace with filter and ranges::to<>
    for(const auto* spec : stmt->specializations()) {
        // Explicit specializations and instantiations will appear later in the AST as dedicated node. Don't
        // generate code for them now, otherwise they are there twice.
        if(TSK_ImplicitInstantiation == spec->getSpecializationKind()) {
            specializations.push_back(spec);
        }
    }

    // Sort specializations by POI to make dependent specializations work.
    ranges::sort(specializations,
                 [](const ClassTemplateSpecializationDecl* a, const ClassTemplateSpecializationDecl* b) {
                     return a->getPointOfInstantiation() < b->getPointOfInstantiation();
                 });

    for(const auto* spec : specializations) {
        InsertArg(spec);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ParenListExpr* stmt)
{
    for(OnceFalse needsComma{}; const auto& expr : stmt->children()) {
        mOutputFormatHelper.AppendComma(needsComma);

        InsertArg(expr);
    }
}
//-----------------------------------------------------------------------------

/// Fill the values of a constant array.
///
/// This is either called by \c InitListExpr (which may contain an offset, as the user already provided certain
/// values) or by \c GetValueOfValueInit.
std::string
CodeGenerator::FillConstantArray(const ConstantArrayType* ct, const std::string& value, const uint64_t startAt)
{
    OutputFormatHelper ret{};

    if(ct) {
        const auto size{std::clamp(GetSize(ct), uint64_t{0}, MAX_FILL_VALUES_FOR_ARRAYS)};

        OnceFalse needsComma{uint64_t{0} != startAt};
        for_each(startAt, size, [&](auto) {
            ret.AppendComma(needsComma);
            ret.Append(value);
        });
    }

    return ret.GetString();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const InitListExpr* stmt)
{
    // At least in case of a requires-clause containing T{} we don't want to get T{{}}.
    RETURN_IF((NoEmptyInitList::Yes == mNoEmptyInitList) and (0 == stmt->getNumInits()));

    WrapInCurliesIfNeeded(not GetInsightsOptions().UseShow2C or
                              (GetInsightsOptions().UseShow2C and
                               ((stmt->getNumInits() > 1) or stmt->getArrayFiller() or
                                ((0 < stmt->getNumInits()) and isa<ImplicitValueInitExpr>(stmt->getInit(0))))),
                          [&]() {
                              mOutputFormatHelper.IncreaseIndent();

                              ForEachArg(stmt->inits(), [&](const auto& init) { InsertArg(init); });

                              if((0 == stmt->getNumInits()) and GetInsightsOptions().UseShow2C) {
                                  if(stmt->getType().getCanonicalType()->isScalarType()) {
                                      mOutputFormatHelper.Append(GetValueOfValueInit(stmt->getType()));
                                      return;
                                  } else {
                                      // for a non scalar type that shoud become a memset?
                                      // A a{}; -> A a; memset(&a, 0, sizef(a));
                                      // mOutputFormatHelper.Append("memset("sv);
                                  }
                              }

                              // If we have a filler, fill the rest of the array with the filler expr.
                              if(const auto* filler = stmt->getArrayFiller()) {
                                  OutputFormatHelper   ofm{};
                                  CodeGeneratorVariant codeGenerator{ofm};
                                  codeGenerator->InsertArg(filler);

                                  const auto ret = FillConstantArray(
                                      dyn_cast_or_null<ConstantArrayType>(stmt->getType().getTypePtrOrNull()),
                                      ofm.GetString(),
                                      stmt->getNumInits());

                                  mOutputFormatHelper.Append(ret);
                              }
                          });

    mOutputFormatHelper.DecreaseIndent();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXDefaultInitExpr* stmt)
{
    const auto* subExpr = stmt->getExpr();

    InsertCurlysIfRequired(subExpr);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXDeleteExpr* stmt)
{
    mOutputFormatHelper.Append(kwDelete);

    if(stmt->isArrayForm()) {
        mOutputFormatHelper.Append("[]"sv);
    }

    mOutputFormatHelper.Append(' ');

    InsertArg(stmt->getArgument());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertConstructorExpr(const auto* stmt)
{
    {
        CONDITIONAL_LAMBDA_SCOPE_HELPER(Decltype, not isa<DecltypeType>(stmt->getType()))

        P0315Visitor dt{*this};
        dt.TraverseType(stmt->getType());

        if(not mLambdaStack.empty()) {
            for(const auto& e : mLambdaStack) {
                RETURN_IF((LambdaCallerType::MemberCallExpr == e.callerType()) and isa<DecltypeType>(stmt->getType()));
            }
        }
    }

    mOutputFormatHelper.Append(GetName(stmt->getType(), Unqualified::Yes));

    const BraceKind braceKind = ValueOr(stmt->isListInitialization(), BraceKind::Curlys, BraceKind::Parens);

    WrapInParensOrCurlys(braceKind, [&]() {
        if(const auto& arguments = stmt->arguments(); not arguments.empty()) {
            ForEachArg(stmt->arguments(), [&](const auto& arg) { InsertArg(arg); });
        }
    });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXConstructExpr* stmt)
{
    InsertConstructorExpr(stmt);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXUnresolvedConstructExpr* stmt)
{
    BackupAndRestore _{mNoEmptyInitList, NoEmptyInitList::Yes};

    InsertConstructorExpr(stmt);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const UnresolvedMemberExpr* stmt)
{
    // InsertArg(stmt->getBase());
    // const std::string op{};  // stmt->isArrow() ? "->" : "."};

    // mOutputFormatHelper.Append(op, stmt->getMemberNameInfo().getAsString());
    mOutputFormatHelper.Append(stmt->getMemberNameInfo().getAsString());

    if(stmt->getNumTemplateArgs()) {
        InsertTemplateArgs(*stmt);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const PackExpansionExpr* stmt)
{
    InsertArg(stmt->getPattern());
    mOutputFormatHelper.Append(kwElipsisSpace);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXFoldExpr* stmt)
{
    auto operatorStr = BinaryOperator::getOpcodeStr(stmt->getOperator());

    WrapInParens([&] {
        // We have a binary NNN fold. If init is nullptr, then it is a unary NNN fold.
        const auto* init = stmt->getInit();

        if(stmt->isLeftFold()) {
            if(init) {
                InsertArg(init);
                mOutputFormatHelper.Append(" "sv, operatorStr, " "sv);
            }

            mOutputFormatHelper.Append(kwElipsisSpace, operatorStr, " "sv);
        }

        InsertArg(stmt->getPattern());

        if(stmt->isRightFold()) {
            mOutputFormatHelper.Append(" "sv, operatorStr, " "sv, kwElipsis);

            if(init) {
                mOutputFormatHelper.Append(" "sv, operatorStr, " "sv);
                InsertArg(init);
            }
        }
    });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXInheritedCtorInitExpr* stmt)
{
    const auto& constructorDecl = *stmt->getConstructor();

    mOutputFormatHelper.Append(GetName(GetDesugarType(stmt->getType()), Unqualified::Yes));
    WrapInParens([&]() {
        mOutputFormatHelper.AppendParameterList(constructorDecl.parameters(),
                                                OutputFormatHelper::NameOnly::Yes,
                                                OutputFormatHelper::GenMissingParamName::Yes);
    });
}
//-----------------------------------------------------------------------------

bool CodeGenerator::InsideDecltype() const
{
    return (not mLambdaStack.empty()) and (LambdaCallerType::Decltype == mLambdaStack.back().callerType());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXPseudoDestructorExpr* stmt)
{
    InsertArg(stmt->getBase());

    mOutputFormatHelper.Append(ArrowOrDot(stmt->isArrow()), "~", GetName(stmt->getDestroyedType()));
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXMemberCallExpr* stmt)
{
    CONDITIONAL_LAMBDA_SCOPE_HELPER(MemberCallExpr, not InsideDecltype())

    InsertArg(stmt->getCallee());

    WrapInParens([&]() { ForEachArg(stmt->arguments(), [&](const auto& arg) { InsertArg(arg); }); });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ParenExpr* stmt)
{
    WrapInParens([&]() { InsertArg(stmt->getSubExpr()); });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXParenListInitExpr* stmt)
{
    WrapInParens([&]() { ForEachArg(stmt->getInitExprs(), [&](const auto& init) { InsertArg(init); }); });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const UnaryOperator* stmt)
{
    StringRef  opCodeName = UnaryOperator::getOpcodeStr(stmt->getOpcode());
    const bool insertBefore{not stmt->isPostfix()};

    if(insertBefore) {
        mOutputFormatHelper.Append(opCodeName);
    }

    InsertArg(stmt->getSubExpr());

    if(not insertBefore) {
        mOutputFormatHelper.Append(opCodeName);
    }
}
//-------------	----------------------------------------------------------------

void CodeGenerator::InsertArg(const StringLiteral* stmt)
{
    StringStream stream{};
    stream.Print(*stmt);

    mOutputFormatHelper.Append(stream.str());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ArrayInitIndexExpr* stmt)
{
    Error(stmt, "ArrayInitIndexExpr should not be reached in CodeGenerator");
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ArraySubscriptExpr* stmt)
{
    if((not GetInsightsOptions().UseAltArraySubscriptionSyntax) or stmt->getLHS()->isLValue()) {
        InsertArg(stmt->getLHS());

        mOutputFormatHelper.Append('[');
        InsertArg(stmt->getRHS());
        mOutputFormatHelper.Append(']');
    } else {

        mOutputFormatHelper.Append("(*("sv);
        InsertArg(stmt->getLHS());
        mOutputFormatHelper.Append(" + "sv);

        InsertArg(stmt->getRHS());
        mOutputFormatHelper.Append("))"sv);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ArrayInitLoopExpr* stmt)
{
    WrapInCurlys([&]() {
        const uint64_t size = stmt->getArraySize().getZExtValue();

        ForEachArg(NumberIterator(size), [&](const auto& i) {
            ArrayInitCodeGenerator codeGenerator{mOutputFormatHelper, i};
            codeGenerator.InsertArg(stmt->getSubExpr());
        });
    });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const OpaqueValueExpr* stmt)
{
    InsertArg(stmt->getSourceExpr());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CallExpr* stmt)
{
    const bool insideDecltype{InsideDecltype()};

    CONDITIONAL_LAMBDA_SCOPE_HELPER(CallExpr, not insideDecltype)
    if(insideDecltype) {
        mLambdaStack.back().setInsertName(true);
    }

    UpdateCurrentPos(mCurrentCallExprPos);

    InsertArg(stmt->getCallee());

    if(const auto* declRefExpr = dyn_cast_or_null<DeclRefExpr>(stmt->getCallee()->IgnoreImpCasts())) {
        if(const auto* fd = dyn_cast_or_null<FunctionDecl>(declRefExpr->getDecl())) {
            if((not declRefExpr->getNumTemplateArgs() and GetInsightsOptions().ShowAllCallExprTemplateParameters) or
               isa<UserDefinedLiteral>(stmt)) {
                InsertTemplateArgs(*fd);
            }
        }
    }

    WrapInParens([&]() {
        auto*    funcDecl = dyn_cast_or_null<FunctionDecl>(stmt->getCalleeDecl());
        unsigned parmIdx{};

        ForEachArg(stmt->arguments(), [&](const auto* arg) {
            ++parmIdx;

            if(const auto* tmpExpr = dyn_cast_or_null<CXXBindTemporaryExpr>(arg)) {
                if(const auto* tmp = dyn_cast_or_null<CXXTemporaryObjectExpr>(tmpExpr->getSubExpr())) {
                    if(GetInsightsOptions().UseShow2C) {
                        // De-reference the argument since we can only pass temporary objects to functions, not
                        // pointers
                        arg = Dref(arg);

                    } else if(GetInsightsOptions().ShowLifetime) {
                        mOutputFormatHelper.Append(GetName(*tmp));
                        return;  // from lambda, which is like a continue
                    }
                }
            }

            if(GetInsightsOptions().UseShow2C and funcDecl and (funcDecl->getNumParams() >= parmIdx) and
               IsReferenceType(funcDecl->getParamDecl(parmIdx - 1))) {
                if(auto* unop = dyn_cast_or_null<UnaryOperator>(arg); not unop or (unop->getOpcode() != UO_AddrOf)) {
                    arg = Ref(arg);
                }
            }

            InsertArg(arg);
        });
    });

    if(insideDecltype) {
        mLambdaStack.back().setInsertName(false);
    }

    mCurrentCallExprPos.reset();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXNamedCastExpr* stmt)
{
    const QualType castDestType = stmt->getTypeAsWritten();
    const Expr*    subExpr      = stmt->getSubExpr();

    FormatCast(stmt->getCastName(), castDestType, subExpr, stmt->getCastKind());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ImplicitCastExpr* stmt)
{
    const Expr* subExpr  = stmt->getSubExpr();
    const auto  castKind = stmt->getCastKind();
    const bool  hideImplicitCasts{not GetInsightsOptions().ShowAllImplicitCasts};

    auto isMatchingCast = [](const CastKind kind, const bool hideImplicitCasts, const bool showXValueCasts) {
        switch(kind) {
            case CastKind::CK_Dependent: [[fallthrough]];
            case CastKind::CK_IntegralCast: [[fallthrough]];
            case CastKind::CK_IntegralToBoolean: [[fallthrough]];
            case CastKind::CK_IntegralToPointer: [[fallthrough]];
            case CastKind::CK_PointerToIntegral: [[fallthrough]];
            case CastKind::CK_BitCast: [[fallthrough]];
            case CastKind::CK_UncheckedDerivedToBase: [[fallthrough]];
            case CastKind::CK_ToUnion:
                [[fallthrough]];
                //            case CastKind::CK_UserDefinedConversion: [[fallthrough]];
            case CastKind::CK_AtomicToNonAtomic: [[fallthrough]];
            case CastKind::CK_DerivedToBase: [[fallthrough]];
            case CastKind::CK_FloatingCast: [[fallthrough]];
            case CastKind::CK_IntegralToFloating: [[fallthrough]];
            case CastKind::CK_FloatingToIntegral: [[fallthrough]];
            case CastKind::CK_NonAtomicToAtomic: return true;
            default:
                // Special case for structured bindings
                if((showXValueCasts or not hideImplicitCasts) and (CastKind::CK_NoOp == kind)) {
                    return true;
                }

                // Show this casts only if ShowAllImplicitCasts is turned on.
                if(not hideImplicitCasts) {
                    switch(kind) {
                        case CastKind::CK_NullToPointer: [[fallthrough]];
                        case CastKind::CK_NullToMemberPointer: [[fallthrough]];
                        /* these are implicit conversions. We get them right, but they may end up in a compiler
                         * internal type, which leads to compiler errors */
                        case CastKind::CK_NoOp: [[fallthrough]];
                        case CastKind::CK_ArrayToPointerDecay: return true;
                        default: break;
                    }
                }

                return false;
        }
    };

    if(not isMatchingCast(castKind, hideImplicitCasts, stmt->isXValue() or ShowXValueCasts())) {
        if(GetInsightsOptions().UseShow2C and (castKind == CastKind::CK_LValueToRValue) and
           IsReferenceType(dyn_cast_or_null<DeclRefExpr>(subExpr))) {
            mOutputFormatHelper.Append("*"sv);
        }

        InsertArg(subExpr);

    } else if(isa<IntegerLiteral>(subExpr) and hideImplicitCasts) {
        InsertArg(stmt->IgnoreCasts());

        // If this is part of an explicit cast, for example a CStyleCast or static_cast, ignore it, because it
        // belongs to the cast written by the user.
    } else if(stmt->isPartOfExplicitCast()) {
        InsertArg(stmt->IgnoreCasts());

    } else {
        auto           castName{GetCastName(castKind)};
        const QualType castDestType{[&] {
            const auto type{stmt->getType()};

            // In at least the case a structured bindings the compiler adds xvalue casts but the && is missing to
            // make it valid C++.
            if(VK_XValue == stmt->getValueKind()) {
                return GetGlobalAST().getRValueReferenceType(type.getCanonicalType());
            } else if(type->isDependentType()) {  // In case of a dependent type the canonical type doesn't know the
                                                  // parameters name.
                return type;
            }

            return type.getCanonicalType();
        }()};

        FormatCast(castName, castDestType, subExpr, castKind);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const DeclRefExpr* stmt)
{
    if(const auto* tmplObjParam = dyn_cast_or_null<TemplateParamObjectDecl>(stmt->getDecl())) {
        mOutputFormatHelper.Append(GetName(*tmplObjParam));

    } else if(const auto* vd = dyn_cast_or_null<VarDecl>(stmt->getDecl());
              GetInsightsOptions().UseShow2C and IsReferenceType(vd)) {
        const auto* init = vd->getInit();

        if(const auto* dref = dyn_cast_or_null<DeclRefExpr>(init)) {
            mOutputFormatHelper.Append(GetName(*dref));
            return;

        } else if(const auto* inList = dyn_cast_or_null<InitListExpr>(init)) {
            mOutputFormatHelper.Append(GetName(*dyn_cast_or_null<DeclRefExpr>(inList->getInit(0))));
            return;
        }
    }

    if(const auto* ctx = stmt->getDecl()->getDeclContext(); not ctx->isFunctionOrMethod() and
                                                            not isa<NonTypeTemplateParmDecl>(stmt->getDecl()) and
                                                            not GetInsightsOptions().UseShow2C) {
        if(const auto* qualifier = stmt->getQualifier();
           qualifier and (qualifier->getKind() == NestedNameSpecifier::SpecifierKind::Global)) {
            // According to
            // https://clang.llvm.org/doxygen/classclang_1_1NestedNameSpecifier.html#ac707a113605ed4283684b8c05664eb6f
            // the global specifier is not stored.
            mOutputFormatHelper.Append("::"sv, GetPlainName(*stmt));

        } else {
            OutputFormatHelper   ofm{};
            CodeGeneratorVariant codeGenerator{ofm};

            codeGenerator->ParseDeclContext(ctx);

            mOutputFormatHelper.Append(ScopeHandler::RemoveCurrentScope(ofm.GetString()), GetPlainName(*stmt));
        }

    } else {
        mOutputFormatHelper.Append(GetName(*stmt));
    }

    if(const auto* varTmplSpecDecl = dyn_cast_or_null<VarTemplateSpecializationDecl>(stmt->getDecl())) {
        InsertTemplateArgs(*varTmplSpecDecl);
    } else {
        InsertTemplateArgs(*stmt);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CompoundStmt* stmt)
{
    mOutputFormatHelper.OpenScope();
    mLifeTimeTracker.StartScope(isa_and_nonnull<FunctionDecl>(mLastDecl));

    // prevent nested CompoundStmt's to insert a return on each leave. Only insert it before closing the most outer
    // one.
    const bool requiresImplicitReturnZero{std::exchange(mRequiresImplicitReturnZero, false)};

    HandleCompoundStmt(stmt);

    if(requiresImplicitReturnZero) {
        InsertArg(Return(Int32(0)));

        if(not mSkipSemi) {
            InsertArg(mkNullStmt());
        }
    }

    mSkipSemi = mLifeTimeTracker.EndScope(mOutputFormatHelper, isa_and_nonnull<ReturnStmt>(mLastStmt));

    mOutputFormatHelper.CloseScope(OutputFormatHelper::NoNewLineBefore::Yes);
}
//-----------------------------------------------------------------------------

template<typename... Args>
static bool IsStmtRequiringSemi(const Stmt* stmt)
{
    return (... and not isa<Args>(stmt));
}
//-----------------------------------------------------------------------------

void CodeGenerator::HandleCompoundStmt(const CompoundStmt* stmt)
{
    for(const auto* item : stmt->body()) {
        InsertArg(item);

        // Skip inserting a semicolon, if this is a LambdaExpr and out stack is empty. This addresses a special case
        // #344.
        const bool skipSemiForLambda{mLambdaStack.empty() and isa<LambdaExpr>(item)};

        if(IsStmtRequiringSemi<IfStmt,
                               NullStmt,
                               ForStmt,
                               DeclStmt,
                               WhileStmt,
                               DoStmt,
                               CXXForRangeStmt,
                               SwitchStmt,
                               CXXTryStmt,
                               CppInsightsCommentStmt>(item) and
           InsertSemi() and not skipSemiForLambda and not mSkipSemi) {
            mOutputFormatHelper.AppendSemiNewLine();
        }

        mSkipSemi = false;
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertIfOrSwitchInitVariables(same_as_any_of<const IfStmt, const SwitchStmt> auto* stmt)
{
    if(const auto* conditionVar = stmt->getConditionVariable()) {
        InsertArg(conditionVar);
    }

    if(const auto* init = stmt->getInit()) {
        InsertArg(init);

        if(not isa<DeclStmt>(init)) {
            mOutputFormatHelper.AppendSemiNewLine();
        }
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const IfStmt* stmt)
{
    const bool hasInit{stmt->getInit() or stmt->getConditionVariable()};

    if(hasInit) {
        mOutputFormatHelper.OpenScope();

        InsertIfOrSwitchInitVariables(stmt);
    }

    mOutputFormatHelper.Append("if"sv, ValueOrDefault(stmt->isConstexpr(), kwSpaceConstExpr));

    WrapInParensIfNeeded(
        not stmt->isConsteval(),
        [&]() {
            mShowConstantExprValue = ShowConstantExprValue::Yes;

            InsertArg(stmt->getCond());

            mShowConstantExprValue = ShowConstantExprValue::No;
        },
        AddSpaceAtTheEnd::Yes);

    mOutputFormatHelper.Append(ValueOrDefault(stmt->isNegatedConsteval(), " !"sv),
                               ValueOrDefault(stmt->isConsteval(), kwSpaceConstEvalSpace));

    WrapInCompoundIfNeeded(stmt->getThen(), AddNewLineAfter::No);

    // else
    if(const auto* elsePart = stmt->getElse()) {
        mOutputFormatHelper.Append(
            "else "sv,
            ValueOrDefault(stmt->isConstexpr(), StrCat(kwCCommentStartSpace, kwConstExprSpace, kwCCommentEndSpace)));

        WrapInCompoundIfNeeded(elsePart, AddNewLineAfter::No);
    }

    // Add newline after last closing curly (either from if or else if).
    mOutputFormatHelper.AppendNewLine();

    if(hasInit) {
        mOutputFormatHelper.CloseScope();
        mOutputFormatHelper.AppendNewLine();
    }

    // one blank line after statement
    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

class ContinueASTTransformer : public StmtVisitor<ContinueASTTransformer>
{
    Stmt*            mPrevStmt{};
    std::string_view mContinueLabel{};

public:
    bool found{};

    ContinueASTTransformer(Stmt* stmt, std::string_view continueLabel)
    : mPrevStmt{stmt}
    , mContinueLabel{continueLabel}
    {
        Visit(stmt);
    }

    void Visit(Stmt* stmt)
    {
        if(stmt) {
            StmtVisitor<ContinueASTTransformer>::Visit(stmt);
        }
    }

    void VisitContinueStmt(ContinueStmt* stmt)
    {
        found = true;

        ReplaceNode(mPrevStmt, stmt, Goto(mContinueLabel));
    }

    void VisitStmt(Stmt* stmt)
    {
        auto* tmp = mPrevStmt;
        mPrevStmt = stmt;

        for(auto* child : stmt->children()) {
            Visit(child);
        }

        mPrevStmt = tmp;
    }
};
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ForStmt* stmt)
{
    // https://github.com/vtjnash/clang-ast-builder/blob/master/AstBuilder.cpp
    // http://clang-developers.42468.n3.nabble.com/Adding-nodes-to-Clang-s-AST-td4054800.html
    // https://stackoverflow.com/questions/30451485/how-to-clone-or-create-an-ast-stmt-node-of-clang/38899615

    if(GetInsightsOptions().UseAltForSyntax) {
        auto*          rwStmt = const_cast<ForStmt*>(stmt);
        const auto&    ctx    = GetGlobalAST();
        StmtsContainer bodyStmts{};

        auto       continueLabel = MakeLineColumnName(ctx.getSourceManager(), stmt->getBeginLoc(), "__continue_"sv);
        const bool insertLabel   = ContinueASTTransformer{rwStmt->getBody(), continueLabel}.found;

        bodyStmts.AddBodyStmts(rwStmt->getBody());

        // Build and insert the continue goto label
        if(insertLabel) {
            bodyStmts.Add(Label(continueLabel));
        }

        bodyStmts.Add(rwStmt->getInc());

        auto* condition = [&]() -> Expr* {
            if(rwStmt->getCond()) {
                return rwStmt->getCond();
            }

            return Bool(true);
        }();

        auto* outerBody = mkCompoundStmt(bodyStmts, stmt->getBeginLoc(), stmt->getEndLoc());
        auto* whileStmt = WhileStmt::Create(
            ctx, nullptr, condition, outerBody, stmt->getBeginLoc(), stmt->getLParenLoc(), stmt->getRParenLoc());

        StmtsContainer outerScopeStmts{};
        outerScopeStmts.Add(rwStmt->getInit());
        outerScopeStmts.Add(whileStmt);

        auto* outerScopeBody = mkCompoundStmt(outerScopeStmts, stmt->getBeginLoc(), stmt->getEndLoc());

        InsertArg(outerScopeBody);
        mOutputFormatHelper.AppendNewLine();

    } else {
        {
            // We need to handle the case that a lambda is used in the init-statement of the for-loop.
            LAMBDA_SCOPE_HELPER(VarDecl);

            mOutputFormatHelper.Append("for"sv);

            WrapInParens(
                [&]() {
                    if(const auto* init = stmt->getInit()) {
                        MultiStmtDeclCodeGenerator codeGenerator{
                            mOutputFormatHelper, mLambdaStack, InsertVarDecl(nullptr)};
                        codeGenerator.InsertArg(init);

                    } else {
                        mOutputFormatHelper.Append("; "sv);
                    }

                    InsertArg(stmt->getCond());
                    mOutputFormatHelper.Append("; "sv);

                    InsertArg(stmt->getInc());
                },
                AddSpaceAtTheEnd::Yes);
        }

        WrapInCompoundIfNeeded(stmt->getBody(), AddNewLineAfter::Yes);
    }

    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

static bool IsConstQualifiedType(QualType type)
{
    if(not type.isNull()) {
        if(auto* typePtr = type.getTypePtrOrNull()) {
            if(auto pointee = typePtr->getPointeeType(); not pointee.isNull()) {
                return pointee.isConstQualified();
            }
        }
    }

    return false;
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CStyleCastExpr* stmt)
{
    const auto     castKind     = stmt->getCastKind();
    const QualType castDestType = stmt->getType().getCanonicalType();
    const auto     castName     = GetCastName(
        castKind, IsConstQualifiedType(castDestType) != IsConstQualifiedType(stmt->getSubExpr()->getType()));

    FormatCast(castName, castDestType, stmt->getSubExpr(), castKind);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXNewExpr* stmt)
{
    const auto  noEmptyInitList = mNoEmptyInitList;
    FinalAction _{[&] { mNoEmptyInitList = noEmptyInitList; }};
    mNoEmptyInitList = GetInsightsOptions().UseShow2C ? NoEmptyInitList::Yes : NoEmptyInitList::No;

    mOutputFormatHelper.Append("new "sv);

    if(stmt->getNumPlacementArgs()) {
        /* we have a placement new */

        WrapInParens([&]() {
            ForEachArg(stmt->placement_arguments(), [&](const auto& placementArg) { InsertArg(placementArg); });
        });
    }

    if(const auto* ctorExpr = stmt->getConstructExpr()) {
        InsertArg(ctorExpr);

    } else {
        auto name = GetName(stmt->getAllocatedType());

        // Special handling for arrays. They differ from one to more dimensions.
        if(stmt->isArray()) {
            OutputFormatHelper   ofm{};
            CodeGeneratorVariant codeGenerator{ofm};

            ofm.Append('[');
            codeGenerator->InsertArg(stmt->getArraySize().value());
            ofm.Append(']');

            // In case of multi dimension the first dimension is the getArraySize() while the others are part of the
            // type included in GetName(...).
            if(Contains(name, "["sv)) {
                InsertBefore(name, "["sv, ofm.GetString());
            } else {
                // here we have the single dimension case, the dimension is not part of GetName, so add it.
                name.append(ofm);
            }
        }

        mOutputFormatHelper.Append(name);

        if(stmt->hasInitializer()) {
            InsertCurlysIfRequired(stmt->getInitializer());
        }
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const MaterializeTemporaryExpr* stmt)
{
    // At least in case of a ternary operator wrapped inside a MaterializeTemporaryExpr parens are necessary
    const auto* temporary = stmt->getSubExpr();
    WrapInParensIfNeeded(isa_and_nonnull<ConditionalOperator>(temporary), [&] { InsertArg(temporary); });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXOperatorCallExpr* stmt)
{
    LAMBDA_SCOPE_HELPER(OperatorCallExpr);

    const auto* callee = dyn_cast_or_null<DeclRefExpr>(stmt->getCallee()->IgnoreImpCasts());
    const bool  isCXXMethod{callee and isa<CXXMethodDecl>(callee->getDecl())};

    if(2 == stmt->getNumArgs()) {
        auto getArg = [&](unsigned idx) {
            const auto* arg = stmt->getArg(idx);

            // In show all casts mode don't filter this. It shows how the compiler adds const to arguments, if the
            // argument is non-const but the parameter demands a const object
            if(not GetInsightsOptions().ShowAllImplicitCasts) {
                arg = arg->IgnoreImpCasts();
            }

            return dyn_cast_or_null<DeclRefExpr>(arg);
        };

        const auto* param1 = getArg(0);
        const auto* param2 = getArg(1);

        if(callee and param1 and param2) {
            const std::string replace = [&]() {
                // If the argument is a variable template, add the template arguments to the parameter name.
                auto nameWithTmplArguments = [](const auto param) {
                    return FormatVarTemplateSpecializationDecl(param->getDecl(), GetName(*param));
                };

                if(isa<CXXMethodDecl>(callee->getDecl())) {
                    return StrCat(nameWithTmplArguments(param1),
                                  "."sv,
                                  GetName(*callee),
                                  "("sv,
                                  nameWithTmplArguments(param2),
                                  ")"sv);
                } else {
                    return StrCat(GetName(*callee),
                                  "("sv,
                                  nameWithTmplArguments(param1),
                                  ", "sv,
                                  nameWithTmplArguments(param2),
                                  ")"sv);
                }
            }();

            mOutputFormatHelper.Append(replace);

            return;
        }
    }

    auto        cb           = stmt->child_begin();
    const auto* fallbackArg0 = stmt->getArg(0);

    // arg0 := operator
    // skip arg0
    std::advance(cb, 1);

    const auto* arg1 = *cb;

    std::advance(cb, 1);

    // operators in a namespace but outside a class so operator goes first
    if(not isCXXMethod) {
        // happens for UnresolvedLooupExpr
        if(not callee) {
            if(const auto* adl = dyn_cast_or_null<UnresolvedLookupExpr>(stmt->getCallee())) {
                InsertArg(adl);
            }
        } else {
            mOutputFormatHelper.Append(GetName(*callee));
        }

        mOutputFormatHelper.Append("("sv);
    }

    // insert the arguments
    if(isa<DeclRefExpr>(fallbackArg0)) {
        InsertArgWithParensIfNeeded(fallbackArg0);

    } else {
        InsertArgWithParensIfNeeded(arg1);
    }

    // if it is a class operator the operator follows now
    if(isCXXMethod) {
        const OverloadedOperatorKind opKind = stmt->getOperator();

        const std::string_view operatorKw{ValueOr((OO_Coawait == opKind), kwOperatorSpace, kwOperator)};

        mOutputFormatHelper.Append("."sv, operatorKw, getOperatorSpelling(opKind), "("sv);
    }

    // consume all remaining arguments
    const auto childRange = llvm::make_range(cb, stmt->child_end());

    // at least the call-operator can have more than 2 parameters
    ForEachArg(childRange, [&](const auto& child) {
        if(not isCXXMethod) {
            // in global operators we need to separate the two parameters by comma
            mOutputFormatHelper.Append(", "sv);
        }

        InsertArg(child);
    });

    mOutputFormatHelper.Append(')');
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const LambdaExpr* stmt)
{
    if(not mLambdaStack.empty()) {
        const bool insertName{mLambdaStack.back().insertName()};

        HandleLambdaExpr(stmt, mLambdaStack.back());

        if(insertName) {
            mOutputFormatHelper.Append(GetLambdaName(*stmt));
        }

    } else if(LambdaInInitCapture::Yes == mLambdaInitCapture) {
        LAMBDA_SCOPE_HELPER(InitCapture);
        HandleLambdaExpr(stmt, mLambdaStack.back());
    } else {
        LAMBDA_SCOPE_HELPER(LambdaExpr);
        HandleLambdaExpr(stmt, mLambdaStack.back());
    }

    if(not mLambdaStack.empty()) {
        mLambdaStack.back().insertInits(mOutputFormatHelper);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXThisExpr* stmt)
{
    DPrint("thisExpr: imlicit=%d %s\n", stmt->isImplicit(), GetName(GetDesugarType(stmt->getType())));

    mOutputFormatHelper.Append(kwThis);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXBindTemporaryExpr* stmt)
{
    InsertArg(stmt->getSubExpr());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXFunctionalCastExpr* stmt)
{
    const bool isConstructor{isa<CXXConstructExpr>(stmt->getSubExpr())};
    const bool isStdListInit{isa<CXXStdInitializerListExpr>(stmt->getSubExpr())};
    const bool isListInitialization{stmt->getLParenLoc().isInvalid()};
    const bool needsParens{not isConstructor and not isListInitialization and not isStdListInit};

    // If a constructor follows we do not need to insert the type name. This would insert it twice.
    if(not isConstructor and not isStdListInit) {
        mOutputFormatHelper.Append(GetName(stmt->getTypeAsWritten()));
    }

    WrapInParensIfNeeded(needsParens, [&] { InsertArg(stmt->getSubExpr()); });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXBoolLiteralExpr* stmt)
{
    mOutputFormatHelper.Append(details::ConvertToBoolString(stmt->getValue()));
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const GNUNullExpr* /*stmt*/)
{
    mOutputFormatHelper.Append(kwNull);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CharacterLiteral* stmt)
{
    StringStream stream{};
    stream.Print(*stmt);

    auto str = std::move(stream.str());

    if(str == "'\\x00'"sv) {
        str = "'\\0'"sv;
    } else if(str == "'\\xff'"sv) {
        str = "255"sv;
    }

    mOutputFormatHelper.Append(str);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const PredefinedExpr* stmt)
{
    // Check if getFunctionName returns a valid StringLiteral. It does return a nullptr, if this PredefinedExpr is
    // in a UnresolvedLookupExpr. In that case, print the identifier, e.g. __func__.
    if(const auto* functionName = stmt->getFunctionName()) {
        InsertArg(functionName);
    } else {
        const auto name = PredefinedExpr::getIdentKindName(stmt->getIdentKind());

        mOutputFormatHelper.Append(name);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ExprWithCleanups* stmt)
{
    mLifeTimeTracker.StartScope(false);
    TemporaryDeclFinder temporaryFinder{*this, not mProcessingVarDecl ? stmt->getSubExpr() : nullptr};

    InsertArg(stmt->getSubExpr());

    if(GetInsightsOptions().ShowLifetime and not mProcessingVarDecl) {
        mOutputFormatHelper.AppendSemiNewLine();
    }

    mSkipSemi = mLifeTimeTracker.EndScope(mOutputFormatHelper, false);
}
//-----------------------------------------------------------------------------

std::string CodeGenerator::GetValueOfValueInit(const QualType& t)
{
    const QualType& type = t.getCanonicalType();

    if(type->isScalarType()) {
        switch(type->getScalarTypeKind()) {
            case Type::STK_CPointer:
            case Type::STK_BlockPointer:
            case Type::STK_ObjCObjectPointer:
            case Type::STK_MemberPointer: return std::string{kwNullptr};

            case Type::STK_Bool: return std::string{kwFalse};

            case Type::STK_Integral:
            case Type::STK_Floating:
                if(const auto* bt = type->getAs<BuiltinType>()) {
                    switch(bt->getKind()) {
                            // Type::STK_Integral
                        case BuiltinType::Char_U:
                        case BuiltinType::UChar:
                        case BuiltinType::Char_S:
                        case BuiltinType::SChar: return "'\\0'";
                        case BuiltinType::WChar_U:
                        case BuiltinType::WChar_S: return "L'\\0'";
                        case BuiltinType::Char16: return "u'\\0'";
                        case BuiltinType::Char32: return "U'\\0'";
                        // Type::STK_Floating
                        case BuiltinType::Half:
                        case BuiltinType::Float: return "0.0f";
                        case BuiltinType::Double: return "0.0";
                        default: break;
                    }
                }

                break;

            case Type::STK_FloatingComplex:
            case Type::STK_IntegralComplex:
                if(const auto* complexType = type->getAs<ComplexType>()) {
                    return GetValueOfValueInit(complexType->getElementType());
                }

                break;

            case Type::STK_FixedPoint: Error("STK_FixedPoint is not implemented"); break;
        }

    } else if(const auto* tt = dyn_cast_or_null<ConstantArrayType>(t.getTypePtrOrNull())) {
        const auto&       elementType{tt->getElementType()};
        const std::string elementTypeInitValue{GetValueOfValueInit(elementType)};

        return FillConstantArray(tt, elementTypeInitValue, uint64_t{0});
    }

    return std::string{"0"sv};
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ImplicitValueInitExpr* stmt)
{
    mOutputFormatHelper.Append(GetValueOfValueInit(stmt->getType()));
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXScalarValueInitExpr* stmt)
{
    mOutputFormatHelper.Append(GetName(stmt->getType()), "()"sv);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXTryStmt* stmt)
{
    mOutputFormatHelper.AppendNewLine(kwTrySpace);

    InsertArg(stmt->getTryBlock());

    for(const auto& i : NumberIterator{stmt->getNumHandlers()}) {
        InsertArg(stmt->getHandler(i));
    }

    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXCatchStmt* stmt)
{
    mOutputFormatHelper.Append(" catch"sv);

    WrapInParens(
        [&]() {
            if(not stmt->getCaughtType().isNull()) {
                mOutputFormatHelper.Append(
                    GetTypeNameAsParameter(stmt->getCaughtType(), stmt->getExceptionDecl()->getName()));
            } else {
                mOutputFormatHelper.Append(kwElipsis);
            }
        },
        AddSpaceAtTheEnd::Yes);

    InsertArg(stmt->getHandlerBlock());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXThrowExpr* stmt)
{
    mOutputFormatHelper.Append("throw "sv);

    InsertArg(stmt->getSubExpr());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ConstantExpr* stmt)
{
    if((ShowConstantExprValue::Yes == mShowConstantExprValue) and stmt->hasAPValueResult()) {
        if(const auto value = stmt->getAPValueResult(); value.isInt()) {
            mOutputFormatHelper.Append(value.getInt());
            return;
        }
    }

    InsertArg(stmt->getSubExpr());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const TypeAliasDecl* stmt)
{
    const auto& underlyingType = stmt->getUnderlyingType();

    LAMBDA_SCOPE_HELPER(Decltype);
    P0315Visitor dt{*this};
    dt.TraverseType(underlyingType);

    mOutputFormatHelper.Append(kwUsingSpace, GetName(*stmt), hlpAssing);

    if(auto* templateSpecializationType = underlyingType->getAs<TemplateSpecializationType>()) {
        const bool carriesNamespace{[&] {
            if(const auto tn = templateSpecializationType->getTemplateName();
               (TemplateName::QualifiedTemplate == tn.getKind()) or (TemplateName::DependentTemplate == tn.getKind())) {
                const auto* qtn = tn.getAsQualifiedTemplateName();

                return qtn->getQualifier() != nullptr;
            }

            return false;
        }()};

        if(const auto* elaboratedType = underlyingType->getAs<ElaboratedType>()) {
            if(templateSpecializationType->isSugared() and not carriesNamespace) {
                // do this only if the templateSpecializationType does not carry a nestedns
                InsertNamespace(elaboratedType->getQualifier());
            }
        }

        StringStream stream{};
        stream.Print(*templateSpecializationType);

        mOutputFormatHelper.Append(stream.str());

        InsertTemplateArgs(*templateSpecializationType);
    } else if(auto* dependentTemplateSpecializationType =
                  underlyingType->getAs<DependentTemplateSpecializationType>()) {

        mOutputFormatHelper.Append(GetElaboratedTypeKeyword(dependentTemplateSpecializationType->getKeyword()));

        InsertNamespace(dependentTemplateSpecializationType->getQualifier());

        mOutputFormatHelper.Append(kwTemplateSpace, dependentTemplateSpecializationType->getIdentifier()->getName());

        InsertTemplateArgs(*dependentTemplateSpecializationType);

    } else {
        mOutputFormatHelper.Append(GetName(underlyingType));
    }

    mOutputFormatHelper.AppendSemiNewLine();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const TypedefDecl* stmt)
{
    /* function pointer typedefs are special. Ease up things using "using" */
    //    outputFormatHelper.AppendNewLine("typedef ", GetName(stmt->getUnderlyingType()), " ", GetName(*stmt),
    //    ";");
    mOutputFormatHelper.AppendSemiNewLine(kwUsingSpace, GetName(*stmt), hlpAssing, GetName(stmt->getUnderlyingType()));
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertCXXMethodHeader(const CXXMethodDecl* stmt, OutputFormatHelper& initOutputFormatHelper)
{
    LAMBDA_SCOPE_HELPER(CXXMethodDecl);
    CXXConstructorDecl* cxxInheritedCtorDecl{nullptr};

    // Traverse the ctor inline init statements first to find a potential CXXInheritedCtorInitExpr. This carries the
    // name and the type. The CXXMethodDecl above knows only the type.
    if(const auto* ctor = dyn_cast_or_null<CXXConstructorDecl>(stmt)) {
        CodeGeneratorVariant codeGenerator{initOutputFormatHelper, mLambdaStack, mProcessingPrimaryTemplate};
        codeGenerator->mCurrentVarDeclPos         = mCurrentVarDeclPos;
        codeGenerator->mCurrentFieldPos           = mCurrentFieldPos;
        codeGenerator->mCurrentCallExprPos        = mCurrentCallExprPos;
        codeGenerator->mOutputFormatHelperOutside = &mOutputFormatHelper;

        for(OnceTrue first{}; const auto* init : ctor->inits()) {
            initOutputFormatHelper.AppendNewLine();
            if(first) {
                initOutputFormatHelper.Append(": "sv);
            } else {
                initOutputFormatHelper.Append(", "sv);
            }

            // in case of delegating or base initializer there is no member.
#if 0
            if(const auto* member = init->getMember()) {
                initOutputFormatHelper.Append(member->getName());
                codeGenerator->InsertCurlysIfRequired(init->getInit());
            } else {
                const auto* inlineInit = init->getInit();
                bool        useCurlies{false};

                if(const auto* cxxInheritedCtorInitExpr = dyn_cast_or_null<CXXInheritedCtorInitExpr>(inlineInit)) {
                    cxxInheritedCtorDecl = cxxInheritedCtorInitExpr->getConstructor();

                    // Insert the base class name only, if it is not a CXXContructorExpr and not a
                    // CXXDependentScopeMemberExpr which already carry the type.
                } else if(init->isBaseInitializer() and not isa<CXXConstructExpr>(inlineInit)) {
                    initOutputFormatHelper.Append(GetUnqualifiedScopelessName(init->getBaseClass()));
                    useCurlies = true;
                }

                codeGenerator->WrapInCurliesIfNeeded(useCurlies, [&] { codeGenerator->InsertArg(inlineInit); });
            }
#else
            const auto* inlineInit = init->getInit();

            // in case of delegating or base initializer there is no member.
            if(const auto* member = init->getMember()) {
                initOutputFormatHelper.Append(member->getName());

                if(isa<ParenListExpr>(inlineInit)) {
                    codeGenerator->WrapInParens([&] { codeGenerator->InsertArg(inlineInit); });
                } else {
                    codeGenerator->InsertCurlysIfRequired(inlineInit);
                }

            } else if(const auto* cxxInheritedCtorInitExpr = dyn_cast_or_null<CXXInheritedCtorInitExpr>(inlineInit)) {
                cxxInheritedCtorDecl = cxxInheritedCtorInitExpr->getConstructor();

                codeGenerator->InsertArg(inlineInit);

                // Insert the base class name only, if it is not a CXXContructorExpr and not a
                // CXXDependentScopeMemberExpr which already carry the type.
            } else if(init->isBaseInitializer() and not isa<CXXConstructExpr>(inlineInit)) {
                initOutputFormatHelper.Append(GetUnqualifiedScopelessName(init->getBaseClass()));

                const auto braceKind = isa<ParenListExpr>(inlineInit) ? BraceKind::Parens : BraceKind::Curlys;

                codeGenerator->WrapInParensOrCurlys(braceKind, [&] { codeGenerator->InsertArg(inlineInit); });
            } else {
                codeGenerator->InsertArg(inlineInit);
            }
#endif
        }
    }

    InsertTemplateGuardBegin(stmt);
    InsertFunctionNameWithReturnType(*stmt, cxxInheritedCtorDecl);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertCXXMethodDecl(const CXXMethodDecl* stmt, SkipBody skipBody)
{
    OutputFormatHelper initOutputFormatHelper{};
    initOutputFormatHelper.SetIndent(mOutputFormatHelper, OutputFormatHelper::SkipIndenting::Yes);

    const auto posBeforeFunc = mOutputFormatHelper.CurrentPos();

    InsertCXXMethodHeader(stmt, initOutputFormatHelper);

    if(not stmt->isUserProvided() or stmt->isExplicitlyDefaulted()) {
        InsertTemplateGuardEnd(stmt);
        return;
    }

    mOutputFormatHelper.Append(initOutputFormatHelper);

    if(isa<CXXConversionDecl>(stmt)) {
        if(stmt->getParent()->isLambda() and not stmt->doesThisDeclarationHaveABody()) {
            mOutputFormatHelper.AppendNewLine();
            WrapInCurlys([&]() {
                mOutputFormatHelper.AppendNewLine();
                mOutputFormatHelper.Append("  "sv, kwReturn, " "sv);
                if(const auto* invoker = stmt->getParent()->getLambdaStaticInvoker()) {
                    mOutputFormatHelper.AppendSemiNewLine(invoker->getName());
                } else {
                    mOutputFormatHelper.AppendSemiNewLine(kwOperator, "()"sv);
                }
            });
        }
    }

    if((SkipBody::No == skipBody) and stmt->doesThisDeclarationHaveABody() and not stmt->isLambdaStaticInvoker()) {
        InsertMethodBody(stmt, posBeforeFunc);

    } else if(not InsertLambdaStaticInvoker(stmt) or (SkipBody::Yes == skipBody)) {
        mOutputFormatHelper.AppendSemiNewLine();
    }

    InsertTemplateGuardEnd(stmt);

    if(SkipBody::No == skipBody) {
        mOutputFormatHelper.AppendNewLine();
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXMethodDecl* stmt)
{
    // As per [special]/1: "Programs shall not define implicitly-declared special member functions." hide special
    // members which are not used and with that not fully evaluated. This also hopefully removes confusion about the
    // noexcept, which is not evaluated, if the special member is not used.
    RETURN_IF(not GetInsightsOptions().UseShow2C and not stmt->hasBody() and not stmt->isUserProvided() and
              not stmt->isExplicitlyDefaulted() and not stmt->isDeleted());

    InsertCXXMethodDecl(stmt, SkipBody::No);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const EnumDecl* stmt)
{
    mOutputFormatHelper.Append(kwEnumSpace);

    if(stmt->isScoped()) {
        if(stmt->isScopedUsingClassTag()) {
            mOutputFormatHelper.Append(kwClassSpace);
        } else {
            mOutputFormatHelper.Append(kwStructSpace);
        }
    }

    mOutputFormatHelper.Append(stmt->getName());

    if(stmt->isFixed()) {
        mOutputFormatHelper.Append(" : "sv, GetName(stmt->getIntegerType()));
    }

    mOutputFormatHelper.AppendNewLine();

    WrapInCurlys(
        [&]() {
            mOutputFormatHelper.IncreaseIndent();
            mOutputFormatHelper.AppendNewLine();
            OnceFalse needsComma{};

            ForEachArg(stmt->enumerators(), [&](const auto* value) {
                if(needsComma) {
                    mOutputFormatHelper.AppendNewLine();
                }

                InsertArg(value);
            });

            InsertArg(stmt->getBody());

            mOutputFormatHelper.DecreaseIndent();
            mOutputFormatHelper.AppendNewLine();
        },
        AddSpaceAtTheEnd::No);

    mOutputFormatHelper.AppendSemiNewLine();
    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const EnumConstantDecl* stmt)
{
    mOutputFormatHelper.Append(stmt->getName());

    InsertAttributes(stmt);

    if(const auto* initExpr = stmt->getInitExpr()) {
        mOutputFormatHelper.Append(hlpAssing);

        InsertArg(initExpr);
    }
}
//-----------------------------------------------------------------------------

static auto& GetRecordLayout(const RecordDecl* recordDecl)
{
    return GetGlobalAST().getASTRecordLayout(recordDecl);
}
//-----------------------------------------------------------------------------

// XXX: replace with std::format once it is available in all std-libs
auto GetSpaces(std::string::size_type offset)
{
    static const std::string_view spaces{"                              "sv};

    if(offset >= spaces.size()) {
        return ""sv;
    } else {
        return spaces.substr(0, spaces.size() - offset);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const FieldDecl* stmt)
{
    LAMBDA_SCOPE_HELPER(Decltype);
    P0315Visitor dt{*this};

    auto type = GetType(stmt->getType());
    dt.TraverseType(type);

    const auto initialSize{mOutputFormatHelper.size()};
    InsertAttributes(stmt->attrs());

    if(stmt->isMutable()) {
        mOutputFormatHelper.Append(kwMutableSpace);
    }

    if(const auto* cxxRecordDecl = dyn_cast_or_null<CXXRecordDecl>(stmt->getParent())) {
        std::string name{GetName(*stmt)};

        if(const auto fieldName = GetFieldDeclNameForLambda(*stmt, *cxxRecordDecl)) {
            name = std::move(fieldName.value());
        }

        mOutputFormatHelper.Append(GetTypeNameAsParameter(type, name));

        if(const auto* constantExpr = dyn_cast_or_null<ConstantExpr>(stmt->getBitWidth())) {
            mOutputFormatHelper.Append(':');
            InsertArg(constantExpr);
        }

        // Keep the inline init for aggregates, as we do not see it somewhere else.
        if(const auto* initializer = stmt->getInClassInitializer();
           stmt->hasInClassInitializer() and initializer and cxxRecordDecl->isAggregate()) {
            const bool isConstructorExpr{isa<CXXConstructExpr>(initializer) or isa<ExprWithCleanups>(initializer)};
            if((ICIS_ListInit != stmt->getInClassInitStyle()) or isConstructorExpr) {
                mOutputFormatHelper.Append(hlpAssing);
            }

            InsertArg(initializer);
        }
    }

    mOutputFormatHelper.Append(';');

    if(GetInsightsOptions().UseShowPadding) {
        const auto* fieldClass   = stmt->getParent();
        const auto& recordLayout = GetRecordLayout(fieldClass);
        auto        effectiveFieldSize{GetGlobalAST().getTypeInfoInChars(type).Width.getQuantity()};
        auto        getFieldOffsetInBytes = [&recordLayout](const FieldDecl* field) {
            return recordLayout.getFieldOffset(field->getFieldIndex()) / 8;  // this is in bits
        };
        auto       fieldOffset = getFieldOffsetInBytes(stmt);
        const auto offset      = mOutputFormatHelper.size() - initialSize;

        mOutputFormatHelper.Append(GetSpaces(offset), "  /* offset: "sv, fieldOffset, ", size: "sv, effectiveFieldSize);

        // - get next field
        // - if this fields offset + size is equal to the next fields offset we are good,
        // - if not we insert padding bytes
        // - in case there is no next field this is the last field, check this field's offset + size against the
        // records
        //   size. If unequal padding is needed

        const auto expectedOffset = fieldOffset + effectiveFieldSize;
        const auto nextOffset     = [&]() -> uint64_t {
            // find previous field
            if(const auto next = stmt->getFieldIndex() + 1; recordLayout.getFieldCount() > next) {
                // We are in bounds, means we expect to get back a valid iterator
                const auto* field = *std::next(fieldClass->fields().begin(), next);

                return getFieldOffsetInBytes(field);
            }

            // no field found means we are the last field
            return recordLayout.getSize().getQuantity();
        }();

        if(expectedOffset < nextOffset) {
            const auto padding = nextOffset - expectedOffset;
            mOutputFormatHelper.AppendNewLine();
            std::string s = StrCat("char "sv, BuildInternalVarName("padding"sv), "["sv, padding, "];"sv);
            mOutputFormatHelper.Append(s, GetSpaces(s.length()), "                size: ", padding);
        }

        mOutputFormatHelper.AppendNewLine(" */"sv);

    } else {
        mOutputFormatHelper.AppendNewLine();
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const AccessSpecDecl* stmt)
{
    mOutputFormatHelper.AppendNewLine();
    mOutputFormatHelper.AppendNewLine(AccessToStringWithColon(stmt->getAccess()));
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const StaticAssertDecl* stmt)
{
    LAMBDA_SCOPE_HELPER(CallExpr);

    if(not stmt->isFailed()) {
        mOutputFormatHelper.Append("/* PASSED: "sv);
    } else {
        mOutputFormatHelper.Append("/* FAILED: "sv);
    }

    mOutputFormatHelper.Append(kwStaticAssert);

    WrapInParens([&] {
        BackupAndRestore _{GetInsightsOptionsRW().ShowLifetime, false};
        InsertArg(stmt->getAssertExpr());

        if(stmt->getMessage()) {
            mOutputFormatHelper.Append(", "sv);
            InsertArg(stmt->getMessage());
        }
    });

    mOutputFormatHelper.AppendNewLine(";"sv, kwSpaceCCommentEnd);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const UsingDirectiveDecl* stmt)
{
    // We need this due to a wired case in UsingDeclTest.cpp
    if(const auto& name = GetName(*stmt->getNominatedNamespace()); not name.empty()) {
        mOutputFormatHelper.AppendSemiNewLine(kwUsingSpace, kwNamespaceSpace, name);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const NamespaceDecl* stmt)
{
    SCOPE_HELPER(stmt);

    if(stmt->isInline()) {
        mOutputFormatHelper.Append(kwInlineSpace);
    }

    mOutputFormatHelper.Append(kwNamespace);

    if(not stmt->isAnonymousNamespace()) {
        mOutputFormatHelper.Append(" "sv, stmt->getName());
    }

    InsertAttributes(stmt);

    mOutputFormatHelper.AppendNewLine();

    mOutputFormatHelper.OpenScope();

    for(const auto* decl : stmt->decls()) {
        InsertArg(decl);
    }

    mOutputFormatHelper.CloseScope();
    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

void CodeGenerator::ParseDeclContext(const DeclContext* ctx)
{
    mOutputFormatHelper.Append(GetDeclContext(ctx));
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const UsingDecl* stmt)
{
    OutputFormatHelper ofm{};
    ofm.SetIndent(mOutputFormatHelper, OutputFormatHelper::SkipIndenting::Yes);

    // Skip UsingDecl's which have ConstructorUsingShadowDecl attached. This means that we will create the
    // associated constructors from the base class later. Having this \c using still in the code prevents compiling
    // the transformed code.
    if(stmt->shadow_size()) {
        for(const auto* shadow : stmt->shadows()) {
            RETURN_IF(isa<ConstructorUsingShadowDecl>(shadow));

            if(const auto* shadowUsing = dyn_cast_or_null<UsingShadowDecl>(shadow)) {
                if(const auto* targetDecl = shadowUsing->getTargetDecl(); not isa<TypeAliasDecl>(targetDecl)) {
                    UsingCodeGenerator codeGenerator{ofm};
                    codeGenerator.InsertArg(targetDecl);
                }
            }
        }
    }

    mOutputFormatHelper.Append(kwUsingSpace);

    InsertQualifierAndName(stmt->getDeclName(), stmt->getQualifier(), false);

    mOutputFormatHelper.AppendSemiNewLine();

    // Insert what a using declaration pulled into this scope.
    if(not ofm.empty()) {
        mOutputFormatHelper.AppendNewLine(ofm);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const UnresolvedUsingValueDecl* stmt)
{
    mOutputFormatHelper.Append(kwUsingSpace);

    InsertQualifierAndName(stmt->getDeclName(), stmt->getQualifier(), false);

    mOutputFormatHelper.AppendSemiNewLine(Ellipsis(stmt->isPackExpansion()));
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const NamespaceAliasDecl* stmt)
{
    mOutputFormatHelper.AppendNewLine(
        kwNamespaceSpace, stmt->getDeclName().getAsString(), hlpAssing, GetName(*stmt->getAliasedNamespace()), ";");
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const FriendDecl* stmt)
{
    if(const auto* typeInfo = stmt->getFriendType()) {
        mOutputFormatHelper.AppendSemiNewLine(kwFriendSpace, GetName(typeInfo->getType()));

    } else if(const auto* fd = dyn_cast_or_null<FunctionDecl>(stmt->getFriendDecl())) {
        InsertArg(fd);

    } else if(const auto* fdt = dyn_cast_or_null<FunctionTemplateDecl>(stmt->getFriendDecl())) {
        InsertArg(fdt);

    } else {
        std::string cls{};
        if(const auto* ctd = dyn_cast_or_null<ClassTemplateDecl>(stmt->getFriendDecl())) {
            InsertTemplateParameters(*ctd->getTemplateParameters());

            cls = GetTagDeclTypeName(*ctd->getTemplatedDecl());
        }

        mOutputFormatHelper.AppendSemiNewLine(kwFriendSpace, cls, GetName(*stmt->getFriendDecl()));
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXNoexceptExpr* stmt)
{
    mOutputFormatHelper.Append(kwNoexcept);

    WrapInParens([&] { mOutputFormatHelper.Append(details::ConvertToBoolString(stmt->getValue())); });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXDeductionGuideDecl* stmt)
{
    RETURN_IF(DeductionCandidate::Copy == stmt->getDeductionCandidateKind());

    const bool isImplicit{stmt->isImplicit()};
    const bool noSpecializations = [&] {
        if(const auto* dt = stmt->getDescribedFunctionTemplate()) {
            return dt->specializations().empty();
        }

        return false;
    }();

    // Block compiler generated deduction guides which are _overridden_ by user provided deduction guides.
    RETURN_IF(not stmt->isUsed() and isImplicit and noSpecializations);

    const bool isSpecialization{stmt->isFunctionTemplateSpecialization()};
    const bool needsTemplateGuard{isImplicit or isSpecialization};

    if(needsTemplateGuard) {
        InsertTemplateGuardBegin(stmt);
    }

    const auto* deducedTemplate = stmt->getDeducedTemplate();

    if(isSpecialization) {
        InsertTemplateSpecializationHeader(*stmt);
    } else if(const auto* e = stmt->getDescribedFunctionTemplate()) {
        InsertTemplateParameters(*e->getTemplateParameters());
    }

    mOutputFormatHelper.Append(GetName(*deducedTemplate));

    if(stmt->getNumParams()) {
        WrapInParens([&] { mOutputFormatHelper.AppendParameterList(stmt->parameters()); });
    } else {
        mOutputFormatHelper.Append("()"sv);
    }

    mOutputFormatHelper.AppendSemiNewLine(hlpArrow, GetName(stmt->getReturnType()));

    if(needsTemplateGuard) {
        InsertTemplateGuardEnd(stmt);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertTemplate(const FunctionTemplateDecl* stmt, bool withSpec)
{
    LAMBDA_SCOPE_HELPER(TemplateHead);

    mProcessingPrimaryTemplate = ProcessingPrimaryTemplate::Yes;

    // InsertTemplateParameters(*stmt->getTemplateParameters());
    InsertArg(stmt->getTemplatedDecl());

    mProcessingPrimaryTemplate = ProcessingPrimaryTemplate::No;

    RETURN_IF(not withSpec);

    for(const auto* spec : stmt->specializations()) {
        // For specializations we will see them later
        if(spec->getPreviousDecl()) {
            continue;
        }

        mOutputFormatHelper.AppendNewLine();
        InsertArg(spec);
        mOutputFormatHelper.AppendNewLine();
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const FunctionTemplateDecl* stmt)
{
    InsertTemplate(stmt, true);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const TypeAliasTemplateDecl* stmt)
{
    InsertTemplateParameters(*stmt->getTemplateParameters());

    InsertArg(stmt->getTemplatedDecl());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const AttributedStmt* stmt)
{
    for(const auto& attr : stmt->getAttrs()) {
        InsertAttribute(*attr);
    }

    InsertArg(stmt->getSubStmt());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertAttributes(const Decl* stmt)
{
    if(stmt->hasAttrs()) {
        mOutputFormatHelper.Append(" "sv);

        InsertAttributes(stmt->attrs());
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertAttributes(const Decl::attr_range& attrs)
{
    // attrs required for constinit
    for(const auto& attr : attrs) {
        InsertAttribute(*attr);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertAttribute(const Attr& attr)
{
    // skip this attribute. Clang seems to tag virtual methods with override
    RETURN_IF(attr::Override == attr.getKind());

    // skip this attribute. Clang seems to tag final methods or classes with final
    RETURN_IF(attr::Final == attr.getKind());

    // skip this custom clang attribute
    RETURN_IF(attr::NoInline == attr.getKind());

    // Clang's printPretty misses the parameter pack ellipsis. Hence treat this special case here.
    if(const auto* alignedAttr = dyn_cast_or_null<AlignedAttr>(&attr)) {
        auto insert = [&](const QualType type, const TemplateTypeParmType* tmplTypeParam) {
            mOutputFormatHelper.Append(attr.getSpelling(),
                                       "("sv,
                                       kwAlignof,
                                       "("sv,
                                       GetName(type),
                                       ")"sv,
                                       Ellipsis(tmplTypeParam->isParameterPack()),
                                       ") "sv);
        };

        if(alignedAttr->isAlignmentExpr()) {
            if(const auto* unaryExpr = dyn_cast_or_null<UnaryExprOrTypeTraitExpr>(alignedAttr->getAlignmentExpr())) {
                if(const auto* tmplTypeParam =
                       dyn_cast_or_null<TemplateTypeParmType>(unaryExpr->getArgumentType().getTypePtrOrNull())) {
                    insert(unaryExpr->getArgumentType(), tmplTypeParam);
                    return;
                }
            }
        } else if(const auto* tmplTypeParam =
                      alignedAttr->getAlignmentType()->getType()->getAs<TemplateTypeParmType>()) {
            insert(alignedAttr->getAlignmentType()->getType(), tmplTypeParam);
            return;
        }
    }

    StringStream   stream{};
    PrintingPolicy pp{GetGlobalAST().getLangOpts()};
    pp.adjustForCPlusPlus();

    attr.printPretty(stream, pp);

    // attributes start with a space, skip it as it is not required for the first attribute
    std::string_view start{stream.str()};

    mOutputFormatHelper.Append(start, " "sv);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXRecordDecl* stmt)
{
    const size_t insertPosBeforeClass{mOutputFormatHelper.CurrentPos()};
    const auto   indentAtInsertPosBeforeClass{mOutputFormatHelper.GetIndent()};

    SCOPE_HELPER(stmt);

    // Prevent a case like in #205 where the lambda appears twice.
    RETURN_IF(stmt->isLambda() and (mLambdaStack.empty() or (nullptr == mLambdaExpr)));

    const auto* classTemplatePartialSpecializationDecl = dyn_cast_or_null<ClassTemplatePartialSpecializationDecl>(stmt);
    const auto* classTemplateSpecializationDecl        = dyn_cast_or_null<ClassTemplateSpecializationDecl>(stmt);

    // we require the if-guard only if it is a compiler generated specialization. If it is a hand-written variant it
    // should compile.
    const bool isClassTemplateSpecialization{classTemplatePartialSpecializationDecl or classTemplateSpecializationDecl};
    const bool tmplRequiresIfDef{[&] {
        if(classTemplatePartialSpecializationDecl) {
            return classTemplatePartialSpecializationDecl->isImplicit();

        } else if(classTemplateSpecializationDecl) {
            return not classTemplateSpecializationDecl->isExplicitInstantiationOrSpecialization();
        }

        return false;
    }()};

    FinalAction _{[&] {
        if(tmplRequiresIfDef) {
            mOutputFormatHelper.InsertEndIfTemplateGuard();
        }
    }};

    if(isClassTemplateSpecialization) {
        if(tmplRequiresIfDef) {
            InsertInstantiationPoint(GetSM(*classTemplateSpecializationDecl),
                                     classTemplateSpecializationDecl->getPointOfInstantiation());
            mOutputFormatHelper.InsertIfDefTemplateGuard();
        }

        if(classTemplatePartialSpecializationDecl) {
            InsertTemplateParameters(*classTemplatePartialSpecializationDecl->getTemplateParameters());
        } else {
            InsertTemplateSpecializationHeader(*stmt);
        }
        // Render a out-of-line struct declared inside a class template
    } else if(stmt->getLexicalDeclContext() != stmt->getDeclContext()) {
        if(const auto* parent = dyn_cast_or_null<CXXRecordDecl>(stmt->getDeclContext())) {
            if(const auto* outerClasTemplateDecl = parent->getDescribedClassTemplate()) {
                InsertTemplateParameters(*outerClasTemplateDecl->getTemplateParameters());
            }
        }
    }

    mOutputFormatHelper.Append(GetTagDeclTypeName(*stmt));

    InsertAttributes(stmt->attrs());

    mOutputFormatHelper.Append(GetName(*stmt));

    if(classTemplateSpecializationDecl) {
        InsertTemplateArgs(*classTemplateSpecializationDecl);
    }

    if(stmt->hasAttr<FinalAttr>()) {
        mOutputFormatHelper.Append(kwSpaceFinal);
    }

    // skip classes/struct's without a definition
    if(not stmt->hasDefinition() or not stmt->isCompleteDefinition()) {
        mOutputFormatHelper.AppendSemiNewLine();
        return;
    }

    if(stmt->getNumBases()) {
        mOutputFormatHelper.Append(" : "sv);

        ForEachArg(stmt->bases(), [&](const auto& base) {
            mOutputFormatHelper.Append(getAccessSpelling(base.getAccessSpecifier()),
                                       " "sv,
                                       ValueOrDefault(base.isVirtual(), kwVirtualSpace),
                                       GetName(base.getType()),
                                       Ellipsis(base.isPackExpansion()));
        });
    }

    if(GetInsightsOptions().UseShowPadding) {
        const auto& recordLayout = GetRecordLayout(stmt);
        mOutputFormatHelper.AppendNewLine(
            "  /* size: "sv, recordLayout.getSize(), ", align: "sv, recordLayout.getAlignment(), " */"sv);

    } else {
        mOutputFormatHelper.AppendNewLine();
    }

    mOutputFormatHelper.OpenScope();

    if(GetInsightsOptions().UseShowPadding) {
        for(size_t offset{}; const auto& base : stmt->bases()) {
            const auto& baseRecordLayout = GetRecordLayout(base.getType()->getAsRecordDecl());
            const auto  baseVar          = StrCat("/* base ("sv, GetName(base.getType()), ")"sv);
            const auto  size             = baseRecordLayout.getSize().getQuantity();

            mOutputFormatHelper.AppendNewLine(
                baseVar, GetSpaces(baseVar.size()), "     offset: "sv, offset, ", size: "sv, size, " */"sv);

            offset += size;
        }
    }

    UpdateCurrentPos(mCurrentFieldPos);

    OnceTrue        firstRecordDecl{};
    OnceTrue        firstDecl{};
    Decl::Kind      formerKind{};
    AccessSpecifier lastAccess{stmt->isClass() ? AS_private : AS_public};
    for(const auto* d : stmt->decls()) {
        if(isa<CXXRecordDecl>(d) and firstRecordDecl) {
            continue;
        }

        // Insert a newline when the decl kind changes. This for example, inserts a newline when after a FieldDecl
        // we see a CXXMethodDecl.
        if(not firstDecl and (d->getKind() != formerKind)) {
            // mOutputFormatHelper.AppendNewLine();
        }

        if((stmt->isLambda() and isa<CXXDestructorDecl>(d)) and not d->isUsed()) {
            continue;
        }

        // Insert the access modifier, as at least some of the compiler generated classes do not contain an access
        // specifier which results in a default ctor being private if we do not insert the access modifier.
        if(lastAccess != d->getAccess()) {
            lastAccess = d->getAccess();

            // skip inserting an access specifier of our own, if there is a real one coming.
            if(not isa<AccessSpecDecl>(d)) {
                mOutputFormatHelper.AppendNewLine(AccessToStringWithColon(lastAccess));
            }
        }

        InsertArg(d);

        formerKind = d->getKind();
    }

    if(stmt->isLambda()) {
        const LambdaCallerType lambdaCallerType = mLambdaStack.back().callerType();
        const bool             ctorRequired{stmt->capture_size() or stmt->lambdaIsDefaultConstructibleAndAssignable()};

        if(ctorRequired) {
            if(AS_public != lastAccess) {
                mOutputFormatHelper.AppendNewLine();
                // XXX avoid diff in tests. AccessToStringWithColon add "public: " before there was no space.
                const auto       pub{AccessToStringWithColon(AS_public)};
                std::string_view p{pub};
                p.remove_suffix(1);
                mOutputFormatHelper.AppendNewLine(p);
            }

            if(stmt->lambdaIsDefaultConstructibleAndAssignable()) {
                mOutputFormatHelper.Append(kwCppCommentStartSpace);

                if(stmt->hasConstexprDefaultConstructor()) {
                    mOutputFormatHelper.Append(kwCommentStart, kwConstExprSpace, kwCCommentEndSpace);
                }
            }

            mOutputFormatHelper.Append(GetName(*stmt), "("sv);
        }

        SmallVector<std::string, 5> ctorInitializerList{};
        std::string                 ctorArguments{'{'};
        OnceTrue                    firstCtorArgument{};

        auto addToInits =
            [&](std::string_view name, const FieldDecl* fd, bool isThis, const Expr* expr, bool /*useBraces*/) {
                if(firstCtorArgument) {
                } else {
                    mOutputFormatHelper.Append(", "sv);
                    ctorArguments.append(", "sv);
                }

                bool byConstRef{false};
                auto fieldName{isThis ? kwInternalThis : name};
                auto fieldDeclType{fd->getType()};
                bool isMoved{};

                std::string fname = StrCat("_"sv, name);

                // Special handling for lambdas with init captures which contain a move. In such a case, copy the
                // initial move statement and make the variable a &&.
                if(const auto* cxxConstructExpr = dyn_cast_or_null<CXXConstructExpr>(expr);
                   cxxConstructExpr and cxxConstructExpr->getConstructor()->isMoveConstructor()) {

                    OutputFormatHelper             ofm{};
                    LambdaInitCaptureCodeGenerator codeGenerator{ofm, mLambdaStack, name};

                    if(cxxConstructExpr->getNumArgs()) {
                        ForEachArg(cxxConstructExpr->arguments(),
                                   [&](const auto& arg) { codeGenerator.InsertArg(arg); });
                    }

                    fieldDeclType = stmt->getASTContext().getRValueReferenceType(fieldDeclType);

                    fname = ofm;

                    // If it is not an object, check for other conditions why we take the variable by const &/&& in the
                    // ctor
                } else if(not fieldDeclType->isReferenceType() and not fieldDeclType->isAnyPointerType() and
                          not fieldDeclType->isUndeducedAutoType()) {
                    byConstRef                      = true;
                    const auto* exprWithoutImpCasts = expr->IgnoreParenImpCasts();

                    // treat a move of a primitive type
                    if(exprWithoutImpCasts->isXValue()) {
                        byConstRef = false;

                        OutputFormatHelper             ofm{};
                        LambdaInitCaptureCodeGenerator codeGenerator{ofm, mLambdaStack, name};
                        codeGenerator.InsertArg(expr);

                        fname = ofm;

                    } else if(exprWithoutImpCasts
                                  ->isPRValue()  // If we are looking at an rvalue (temporary) we need a const ref
                              or exprWithoutImpCasts->getType().isConstQualified()  // If the captured variable is const
                                                                                    // we can take it only by const ref

                    ) {
                        // this must go before adding the L or R-value reference, otherwise we get T& const instead of
                        // const T&

                        if(exprWithoutImpCasts->isPRValue() and isa<CXXBindTemporaryExpr>(exprWithoutImpCasts) and
                           not exprWithoutImpCasts->getType().isConstQualified()) {
                            fieldDeclType = stmt->getASTContext().getRValueReferenceType(fieldDeclType);
                            EnableGlobalInsert(GlobalInserts::HeaderUtility);
                            fname   = StrCat("std::move("sv, fname, ")"sv);
                            isMoved = true;

                        } else {
                            fieldDeclType.addConst();
                        }
                    }

                    if(exprWithoutImpCasts->isXValue()) {
                        fieldDeclType = stmt->getASTContext().getRValueReferenceType(fieldDeclType);

                    } else if(not isMoved) {
                        fieldDeclType = stmt->getASTContext().getLValueReferenceType(fieldDeclType);
                    }
                }

                const std::string_view elips{
                    Ellipsis(isa_and_nonnull<PackExpansionType>(fieldDeclType->getPointeeType().getTypePtrOrNull()))};

                // To avoid seeing the templates stuff from std::move (typename...) the canonical type is used here.
                fieldDeclType = fieldDeclType.getCanonicalType();

                ctorInitializerList.push_back(StrCat(fieldName, "{"sv, fname, elips, "}"sv));

                if(not isThis and expr) {
                    LAMBDA_SCOPE_HELPER(Decltype);
                    OutputFormatHelper ofmLambdaInCtor{};
                    ofmLambdaInCtor.SetIndent(indentAtInsertPosBeforeClass);
                    CodeGenerator cgLambdaInCtor{ofmLambdaInCtor, LambdaInInitCapture::Yes};

                    if(P0315Visitor dt{cgLambdaInCtor}; dt.TraverseStmt(const_cast<Expr*>(expr))) {

                        OutputFormatHelper   ofm{};
                        CodeGeneratorVariant codeGenerator{ofm, mLambdaStack, mProcessingPrimaryTemplate};

                        if(const auto* ctorExpr = dyn_cast_or_null<CXXConstructExpr>(expr);
                           ctorExpr and byConstRef and (1 == ctorExpr->getNumArgs())) {
                            codeGenerator->InsertArg(ctorExpr->getArg(0));

                        } else {
                            codeGenerator->InsertArg(expr);
                        }

                        //        if(isa<PackExpansionType>(stmt->getDecl()->getType().getTypePtrOrNull())) {
                        //            mOutputFormatHelper.Append(kwElipsisSpace);
                        //        }

                        ctorArguments.append(ofm);

                    } else {
                        OutputFormatHelper          ofm{};
                        LambdaNameOnlyCodeGenerator ccg{ofm};
                        ccg.InsertArg(expr);

                        ctorArguments.append(ofm.GetString());

                        mOutputFormatHelper.InsertAt(insertPosBeforeClass, ofmLambdaInCtor);
                    }
                } else {
                    if(isThis and not fieldDeclType->isPointerType()) {
                        ctorArguments.append("*"sv);
                    }

                    ctorArguments.append(name);
                }

                mOutputFormatHelper.Append(GetTypeNameAsParameter(fieldDeclType, StrCat("_"sv, name)));
            };

        llvm::DenseMap<const ValueDecl*, FieldDecl*> captures{};
        FieldDecl*                                   thisCapture{};

        stmt->getCaptureFields(captures, thisCapture);

        // Check if it captures this
        if(thisCapture) {
            const auto* captureInit = mLambdaExpr->capture_init_begin();

            addToInits(kwThis, thisCapture, true, *captureInit, false);
        }

        // Find the corresponding capture in the DenseMap. The DenseMap seems to be change its order each time.
        // Hence we use \c captures() to keep the order stable. While using \c Captures to generate the code as
        // it carries the better type infos.
        for(const auto& [c, cinit] : zip(mLambdaExpr->captures(), mLambdaExpr->capture_inits())) {
            if(not c.capturesVariable()) {
                continue;
            }

            const auto* capturedVar = c.getCapturedVar();
            if(const auto* value = captures[capturedVar]) {
                // Since C++20 lambdas can capture structured bindings
                if(const auto* bindingDecl = dyn_cast_or_null<BindingDecl>(capturedVar)) {
                    capturedVar = bindingDecl->getHoldingVar();
                }

                addToInits(GetName(*capturedVar),
                           value,
                           false,
                           cinit,
                           VarDecl::ListInit == dyn_cast_or_null<VarDecl>(capturedVar)->getInitStyle());
            }
        }

        ctorArguments.append("}"sv);

        // generate the ctor only if it is required, i.e. we have captures. This is in fact a trick to get
        // compiling code out of it. The compiler itself does not generate a constructor in many many cases.
        if(ctorRequired) {
            mOutputFormatHelper.Append(")"sv);

            if(stmt->lambdaIsDefaultConstructibleAndAssignable()) {
                mOutputFormatHelper.AppendNewLine(kwSpaceEqualsDefault);

            } else {
                mOutputFormatHelper.AppendNewLine();

                for(OnceTrue firstCtorInitializer{}; const auto& initializer : ctorInitializerList) {
                    if(firstCtorInitializer) {
                        mOutputFormatHelper.Append(": "sv);
                    } else {
                        mOutputFormatHelper.Append(", "sv);
                    }

                    mOutputFormatHelper.AppendNewLine(initializer);
                }

                mOutputFormatHelper.AppendNewLine("{}"sv);
            }
        }

        // close the class scope
        mOutputFormatHelper.CloseScope();

        if(not is{lambdaCallerType}.any_of(LambdaCallerType::VarDecl,
                                           LambdaCallerType::InitCapture,
                                           LambdaCallerType::CallExpr,
                                           LambdaCallerType::MemberCallExpr,
                                           LambdaCallerType::TemplateHead,
                                           LambdaCallerType::Decltype)) {
            mOutputFormatHelper.Append(" "sv, GetLambdaName(*stmt), ctorArguments);
        } else if(not is{lambdaCallerType}.any_of(LambdaCallerType::TemplateHead, LambdaCallerType::Decltype)) {
            mLambdaStack.back().inits().append(ctorArguments);
        }
    } else {
        mOutputFormatHelper.CloseScope(OutputFormatHelper::NoNewLineBefore::Yes);
    }

    if(GetInsightsOptions().UseShow2C) {
        mOutputFormatHelper.Append(" "sv, GetName(*stmt));
    }

    mOutputFormatHelper.AppendSemiNewLine();
    mOutputFormatHelper.AppendNewLine();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const DeclStmt* stmt)
{
    for(const auto* decl : stmt->decls()) {
        InsertArg(decl);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const SubstNonTypeTemplateParmExpr* stmt)
{
    InsertArg(stmt->getReplacement());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const SizeOfPackExpr* stmt)
{
    if(stmt->isPartiallySubstituted()) {
        mOutputFormatHelper.Append(stmt->getPartialArguments().size());
    } else if(not stmt->isValueDependent()) {
        mOutputFormatHelper.Append(stmt->getPackLength());
    } else {
        mOutputFormatHelper.Append(kwSizeof, kwElipsis, "("sv, GetName(*stmt->getPack()), ")"sv);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ReturnStmt* stmt)
{
    LAMBDA_SCOPE_HELPER(ReturnStmt);

    UpdateCurrentPos(mCurrentReturnPos);

    {  // dedicated scope to first clear everything found in the return statement. Then clear all others.
        TemporaryDeclFinder temporaryFinder{*this, stmt->getRetValue(), true};

        mOutputFormatHelper.Append(kwReturn);

        if(const auto* retVal = stmt->getRetValue()) {
            mOutputFormatHelper.Append(' ');

            if(not temporaryFinder.Found()) {
                if(const auto* nrvoVD = stmt->getNRVOCandidate()) {
                    mOutputFormatHelper.Append(GetName(*nrvoVD));
                } else {
                    InsertArg(retVal);
                }
            } else {
                mOutputFormatHelper.Append(temporaryFinder.Name());
            }
        }
    }

    mSkipSemi = mLifeTimeTracker.Return(mOutputFormatHelper);

    // the InsertArg above changes the start
    mLastStmt = stmt;

    mCurrentReturnPos.reset();
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const NullStmt* /*stmt*/)
{
    mOutputFormatHelper.AppendSemiNewLine();
    mSkipSemi = true;
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const StmtExpr* stmt)
{
    WrapInParens([&] { InsertArg(stmt->getSubStmt()); });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CppInsightsCommentStmt* stmt)
{
    mOutputFormatHelper.AppendCommentNewLine(stmt->Comment());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const ConceptSpecializationExpr* stmt)
{
    if(const auto* namedConcept = stmt->getNamedConcept()) {
        mOutputFormatHelper.Append(GetName(*namedConcept));
        InsertTemplateArgs(stmt->getTemplateArgsAsWritten()->arguments());

#if 0
        if(not stmt->isValueDependent()) {
            mOutputFormatHelper.Append(kwCCommentStartSpace, stmt->isSatisfied(), kwSpaceCCommentEndSpace);
        }
#endif
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const RequiresExpr* stmt)
{
    mOutputFormatHelper.Append(kwRequires);

    const auto localParameters = stmt->getLocalParameters();
    WrapInParensIfNeeded(
        not localParameters.empty(),
        [&] { mOutputFormatHelper.AppendParameterList(localParameters); },
        AddSpaceAtTheEnd::Yes);

    mOutputFormatHelper.OpenScope();

    const auto  noEmptyInitList = mNoEmptyInitList;
    FinalAction _{[&] { mNoEmptyInitList = noEmptyInitList; }};
    mNoEmptyInitList = NoEmptyInitList::Yes;

    for(const auto& requirement : stmt->getRequirements()) {
        if(const auto* typeRequirement = dyn_cast_or_null<concepts::TypeRequirement>(requirement)) {
            if(typeRequirement->isSubstitutionFailure()) {
                mOutputFormatHelper.Append(kwRequiresSpace, kwFalse);
            } else {
                mOutputFormatHelper.Append(GetName(typeRequirement->getType()->getType()));
            }

            // SimpleRequirement
        } else if(const auto* exprRequirement = dyn_cast_or_null<concepts::ExprRequirement>(requirement)) {
            if(exprRequirement->isExprSubstitutionFailure()) {
                // The requirement failed. We need some way to express that. Using a nested
                // requirement with false seems to be the simplest solution.
                mOutputFormatHelper.Append(kwRequiresSpace, kwFalse);
            } else {
                WrapInCurliesIfNeeded(exprRequirement->isCompound(), [&] { InsertArg(exprRequirement->getExpr()); });

                if(exprRequirement->hasNoexceptRequirement()) {
                    mOutputFormatHelper.Append(kwSpaceNoexcept);
                }

                if(const auto& returnTypeRequirement = exprRequirement->getReturnTypeRequirement();
                   not returnTypeRequirement.isEmpty()) {
                    if(auto typeConstraint = GetTypeConstraintAsString(returnTypeRequirement.getTypeConstraint());
                       not typeConstraint.empty()) {
                        mOutputFormatHelper.Append(hlpArrow, std::move(typeConstraint));
                    }
                }
            }
        } else if(const auto* nestedRequirement = dyn_cast_or_null<concepts::NestedRequirement>(requirement)) {
            mOutputFormatHelper.Append(kwRequiresSpace);

            if(nestedRequirement->hasInvalidConstraint()) {
                // The requirement failed. We need some way to express that. Using a nested
                // requirement with false seems to be the simplest solution.
                mOutputFormatHelper.Append(kwFalse);
            } else {
                InsertArg(nestedRequirement->getConstraintExpr());
            }
        }

        mOutputFormatHelper.AppendSemiNewLine();
    }

    mOutputFormatHelper.CloseScope(OutputFormatHelper::NoNewLineBefore::Yes);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXDefaultArgExpr* stmt)
{
    InsertArg(stmt->getExpr());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXStdInitializerListExpr* stmt)
{
    // No qualifiers like const or volatile here. This appears in  function calls or operators as a parameter.
    // CV's are not allowed there.
    const auto typeName{GetName(stmt->getType(), Unqualified::Yes)};

    if(GetInsightsOptions().UseShowInitializerList) {
        RETURN_IF(not mCurrentVarDeclPos.has_value() and not mCurrentFieldPos.has_value() and
                  not mCurrentReturnPos.has_value() and not mCurrentCallExprPos.has_value());

        const auto* subExpr = stmt->getSubExpr();

        if(const auto* dref = dyn_cast_or_null<DeclRefExpr>(subExpr); dref and GetInsightsOptions().ShowLifetime) {
            const auto size = GetSize(dyn_cast_or_null<ConstantArrayType>(subExpr->getType()));

            mOutputFormatHelper.Append(typeName, "{"sv, GetName(*dref), ", "sv, size, "}"sv);
            return;
        }

        std::string modifiers{};
        size_t      variableInsertPos = mCurrentReturnPos.value_or(
            mCurrentVarDeclPos.value_or(mCurrentCallExprPos.value_or(0)));  // order is important!

        auto& ofmToInsert = [&]() -> decltype(auto) {
            if(not mCurrentVarDeclPos.has_value() and not mCurrentReturnPos.has_value() and
               not mCurrentCallExprPos.has_value()) {
                variableInsertPos  = mCurrentFieldPos.value_or(0);
                mCurrentVarDeclPos = variableInsertPos;
                modifiers          = StrCat(kwStaticSpace, kwInlineSpace);
                return (*mOutputFormatHelperOutside);
            }

            return (mOutputFormatHelper);
        }();

        OutputFormatHelper ofm{};
        ofm.SetIndent(ofmToInsert, OutputFormatHelper::SkipIndenting::Yes);

        const auto size = [&]() -> size_t {
            if(const auto* mat = dyn_cast<MaterializeTemporaryExpr>(subExpr)) {
                if(const auto* list = dyn_cast_or_null<InitListExpr>(mat->getSubExpr())) {
                    return list->getNumInits();
                }
            }

            return 0;
        }();

        auto internalListName =
            MakeLineColumnName(GetGlobalAST().getSourceManager(), stmt->getBeginLoc(), BuildInternalVarName("list"sv));

        ofm.Append(modifiers, GetTypeNameAsParameter(subExpr->getType(), internalListName));
        CodeGeneratorVariant codeGenerator{ofm};
        codeGenerator->InsertArg(subExpr);
        ofm.AppendSemiNewLine();

        ofmToInsert.InsertAt(variableInsertPos, ofm);

        mOutputFormatHelper.Append(typeName, "{"sv, internalListName, ", "sv, size, "}"sv);

        if(mCurrentReturnPos.has_value()) {
            mCurrentReturnPos = mCurrentReturnPos.value() + ofm.size();
        } else if(mCurrentVarDeclPos.has_value()) {
            mCurrentVarDeclPos = mCurrentVarDeclPos.value() + ofm.size();
        } else {
            mCurrentCallExprPos = mCurrentCallExprPos.value() + ofm.size();
        }

    } else {
        mOutputFormatHelper.Append(typeName);
        InsertArg(stmt->getSubExpr());
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const CXXNullPtrLiteralExpr* /*stmt*/)
{
    mOutputFormatHelper.Append(kwNullptr);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const LabelDecl* stmt)
{
    mOutputFormatHelper.Append(stmt->getName());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const Decl* stmt)
{
    mLastDecl = stmt;

#define SUPPORTED_DECL(type)                                                                                           \
    if(isa<type>(stmt)) {                                                                                              \
        InsertArg(static_cast<const type*>(stmt));                                                                     \
        return;                                                                                                        \
    }

#define IGNORED_DECL SUPPORTED_DECL

#include "CodeGeneratorTypes.h"

    ToDo(stmt, mOutputFormatHelper);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const Stmt* stmt)
{
    if(not stmt) {
        DPrint("Null stmt\n");
        return;
    }

    mLastStmt = stmt;

#define SUPPORTED_STMT(type)                                                                                           \
    if(isa<type>(stmt)) {                                                                                              \
        InsertArg(dyn_cast_or_null<type>(stmt));                                                                       \
        return;                                                                                                        \
    }

#define IGNORED_STMT SUPPORTED_STMT

#include "CodeGeneratorTypes.h"

    ToDo(stmt, mOutputFormatHelper);
}
//-----------------------------------------------------------------------------

void CodeGenerator::FormatCast(const std::string_view castName,
                               const QualType&        castDestType,
                               const Expr*            subExpr,
                               const CastKind&        castKind)
{
    const bool        isCastToBase{is{castKind}.any_of(CK_DerivedToBase, CK_UncheckedDerivedToBase) and
                            castDestType->isRecordType()};
    const std::string castDestTypeText{
        StrCat(GetName(castDestType), ((isCastToBase and not castDestType->isAnyPointerType()) ? "&"sv : ""sv))};

    mOutputFormatHelper.Append(castName, "<"sv, castDestTypeText, ">("sv);
    InsertArg(subExpr);
    mOutputFormatHelper.Append(')');
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArgWithParensIfNeeded(const Stmt* stmt)
{
    const bool needsParens = [&]() {
        if(const auto* expr = dyn_cast_or_null<Expr>(stmt)) {
            if(const auto* dest = dyn_cast_or_null<UnaryOperator>(expr->IgnoreImplicit())) {
                return (dest->getOpcode() == clang::UO_Deref);
            }
        }

        return false;
    }();

    WrapInParensIfNeeded(needsParens, [&] { InsertArg(stmt); });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertSuffix(const QualType& type)
{
    if(const auto* typePtr = type.getTypePtrOrNull(); typePtr and typePtr->isBuiltinType()) {
        if(const auto* bt = dyn_cast_or_null<BuiltinType>(typePtr)) {
            const auto kind = bt->getKind();

            mOutputFormatHelper.Append(GetBuiltinTypeSuffix(kind));
        }
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertTemplateArgs(const ClassTemplateSpecializationDecl& clsTemplateSpe)
{
    if(const auto* ar = clsTemplateSpe.getTemplateArgsAsWritten()) {
        InsertTemplateArgs(ar->arguments());
    } else {
        InsertTemplateArgs(clsTemplateSpe.getTemplateArgs());
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::HandleTemplateParameterPack(const ArrayRef<TemplateArgument>& args)
{
    ForEachArg(args, [&](const auto& arg) { InsertTemplateArg(arg); });
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertTemplateArg(const TemplateArgument& arg)
{
    switch(arg.getKind()) {
        case TemplateArgument::Type: mOutputFormatHelper.Append(GetName(arg.getAsType())); break;
        case TemplateArgument::Declaration:
            // TODO: handle pointers
            if(const auto decl = dyn_cast_or_null<TemplateParamObjectDecl>(arg.getAsDecl())) {
                mOutputFormatHelper.Append(GetName(*decl));
            } else {
                mOutputFormatHelper.Append("&"sv, GetName(*arg.getAsDecl(), QualifiedName::Yes));
            }
            break;
        case TemplateArgument::NullPtr: mOutputFormatHelper.Append(kwNullptr); break;
        case TemplateArgument::Integral:

            if(const auto& integral = arg.getAsIntegral(); arg.getIntegralType()->isCharType()) {
                const char c{static_cast<char>(integral.getZExtValue())};
                mOutputFormatHelper.Append("'"sv, std::string{c}, "'"sv);
            } else {
                mOutputFormatHelper.Append(integral);
            }

            break;
        case TemplateArgument::Expression: {
            if(auto val = EvaluateNTTPAsConstantExpr(arg.getAsExpr()->IgnoreParenImpCasts())) {
                mOutputFormatHelper.Append(
                    GetName(val->first),
                    BuildTemplateParamObjectName(val->second.getAsString(GetGlobalAST(), val->first)));
            } else {
                InsertArg(arg.getAsExpr());
            }
        }

        break;
        case TemplateArgument::Pack: HandleTemplateParameterPack(arg.pack_elements()); break;
        case TemplateArgument::Template:
            mOutputFormatHelper.Append(GetName(*arg.getAsTemplate().getAsTemplateDecl()));
            break;
        case TemplateArgument::TemplateExpansion:
            mOutputFormatHelper.Append(GetName(*arg.getAsTemplateOrTemplatePattern().getAsTemplateDecl()));
            break;
        case TemplateArgument::Null: mOutputFormatHelper.Append("null"sv); break;
        case TemplateArgument::StructuralValue: mOutputFormatHelper.Append(arg.getAsStructuralValue()); break;
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::HandleLocalStaticNonTrivialClass(const VarDecl* stmt)
{
    EnableGlobalInsert(GlobalInserts::HeaderNew);

    const auto& ctx = GetGlobalAST();

    auto&      langOpts{GetLangOpts(*stmt)};
    const bool threadSafe{langOpts.ThreadsafeStatics and langOpts.CPlusPlus11 and
                          (stmt->isLocalVarDecl() /*|| NonTemplateInline*/) and not stmt->getTLSKind()};

    const std::string internalVarName{BuildInternalVarName(GetName(*stmt))};
    const std::string compilerBoolVarName{StrCat(internalVarName, "Guard"sv)};

    // insert compiler bool to track init state
    auto* compilerGuardVar =
        Variable(compilerBoolVarName, threadSafe ? Typedef("uint64_t"sv, ctx.UnsignedLongTy) : ctx.BoolTy);
    compilerGuardVar->setStorageClass(StorageClass::SC_Static);
    InsertArg(compilerGuardVar);

    // insert compiler memory place holder
    auto* compilerStorageVar = Variable(internalVarName,
                                        ctx.getConstantArrayType(ctx.CharTy,
                                                                 llvm::APInt(ctx.getTypeSize(ctx.getSizeType()), 0),
                                                                 Sizeof(stmt->getType()),
                                                                 ArraySizeModifier::Normal,
                                                                 0));

    compilerStorageVar->setStorageClass(StorageClass::SC_Static);

    auto* alignedAttr =
        AlignedAttr::CreateImplicit(const_cast<ASTContext&>(ctx),
                                    true,
                                    Sizeof(stmt->getType()),  // ctx.getTrivialTypeSourceInfo(stmt->getType()),
                                    {},
                                    AlignedAttr::Spelling::Keyword_alignas);

    compilerStorageVar->addAttr(alignedAttr);

    const std::string typeName{GetName(stmt->getType())};
    mOutputFormatHelper.AppendSemiNewLine(
        "alignas("sv, typeName, ") static char "sv, internalVarName, "[sizeof("sv, typeName, ")]"sv);

    // insert compiler init if
    mOutputFormatHelper.AppendNewLine();

    // try to find out whether this ctor or the CallExpr can throw. If, then additional code needs to be generated
    // for exception handling.
    const bool canThrow{[&] {
        const ValueDecl* decl = [&]() -> const ValueDecl* {
            const auto* init = stmt->getInit()->IgnoreCasts();
            if(const auto* ctorExpr = dyn_cast_or_null<CXXConstructExpr>(init)) {
                return ctorExpr->getConstructor();
            } else if(const auto* callExpr = dyn_cast_or_null<CallExpr>(init)) {
                return callExpr->getDirectCallee();
            }

            return nullptr;
        }();

        if(decl) {
            if(const auto* func = decl->getType()->castAs<FunctionProtoType>()) {
                return not func->isNothrow();
            }
        }

        return false;
    }()};

    // VarDecl of a static expression always have an initializer
    auto* init = const_cast<Expr*>(stmt->getInit());

    if(const bool isCallExpr{not isa<CXXConstructExpr>(init->IgnoreCasts())}; isCallExpr) {
        // we have a function call
        init = Call("std::move"sv, {init});

        // Tests show that the compiler does better than std::move
        EnableGlobalInsert(GlobalInserts::HeaderUtility);
    }

    // the allocation and guard update:
    // new (&__s)T();
    //  __sGuard = true;
    auto type = stmt->getType();
    type.removeLocalConst();  // Issue369.cpp is a const static variable. Should the ctor use the const?
    SmallVector<Stmt*, 4> allocAndFlagBodyStmts{New({Ref(compilerStorageVar)}, init, type),
                                                Assign(compilerGuardVar, Bool(true))};
    auto                  allocAndFlagBodyCompound = mkCompoundStmt(allocAndFlagBodyStmts);

    StmtsContainer innerBodyStmts{};

    // Need to insert a try catch, if the constructor/initializer can throw
    if(canThrow) {
        innerBodyStmts.AddBodyStmts(
            Try(allocAndFlagBodyCompound, Catch({Call("__cxa_guard_abort"sv, {Ref(compilerGuardVar)}), Throw()})));
    } else {
        innerBodyStmts.AddBodyStmts(allocAndFlagBodyCompound);
    }

    StmtsContainer bodyStmts{};

    if(threadSafe) {
        innerBodyStmts.AddBodyStmts(Call("__cxa_guard_release"sv, {Ref(compilerGuardVar)}));
        innerBodyStmts.Add(Comment(
            StrCat("__cxa_atexit("sv, typeName, "::~"sv, typeName, ", &"sv, internalVarName, ", &__dso_handle);"sv)));

        auto* aquireIf = If(Call("__cxa_guard_acquire"sv, {Ref(compilerGuardVar)}), innerBodyStmts);
        bodyStmts.AddBodyStmts(aquireIf);
    } else {
        bodyStmts.AddBodyStmts(allocAndFlagBodyCompound);
    }

    InsertArg(If(Equal(And(compilerGuardVar, Int32(0xff)), Int32(0)), bodyStmts));
}
//-----------------------------------------------------------------------------

std::string_view CodeGenerator::GetBuiltinTypeSuffix(const BuiltinType::Kind& kind)
{
#define CASE(K, retVal)                                                                                                \
    case BuiltinType::K: return retVal
    switch(kind) {
        CASE(UInt, "U"sv);
        CASE(ULong, "UL"sv);
        CASE(ULongLong, "ULL"sv);
        CASE(UInt128, "ULLL"sv);
        CASE(Long, "L"sv);
        CASE(LongLong, "LL"sv);
        CASE(Float, "F"sv);
        CASE(LongDouble, "L"sv);
        default: return {};
    }
#undef BTCASE
}
//-----------------------------------------------------------------------------

void CodeGenerator::HandleLambdaExpr(const LambdaExpr* lambda, LambdaHelper& lambdaHelper)
{
    OutputFormatHelper& outputFormatHelper = lambdaHelper.buffer();

    outputFormatHelper.AppendNewLine();
    LambdaCodeGenerator codeGenerator{outputFormatHelper, mLambdaStack, mProcessingPrimaryTemplate};
    codeGenerator.mCapturedThisAsCopy = ranges::any_of(
        lambda->captures(), [](auto& c) { return (c.capturesThis() and (c.getCaptureKind() == LCK_StarThis)); });

    codeGenerator.mLambdaExpr = lambda;
    codeGenerator.InsertArg(lambda->getLambdaClass());
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertConceptConstraint(const llvm::SmallVectorImpl<const Expr*>& constraints,
                                            const InsertInline                        insertInline)
{
    for(OnceTrue first{}; const auto* c : constraints) {
        if(first and (InsertInline::Yes == insertInline)) {
            mOutputFormatHelper.Append(' ');
        }

        mOutputFormatHelper.Append(kwRequiresSpace);
        InsertArg(c);

        if(InsertInline::No == insertInline) {
            mOutputFormatHelper.AppendNewLine();
        }
    }
}
//-----------------------------------------------------------------------------

// This inserts the requires clause after template<...>
void CodeGenerator::InsertConceptConstraint(const TemplateParameterList& tmplDecl)
{
    if(const auto* reqClause = tmplDecl.getRequiresClause()) {
        SmallVector<const Expr*, 1> constraints{reqClause};

        InsertConceptConstraint(constraints, InsertInline::No);
    }
}
//-----------------------------------------------------------------------------

// This inserts the requires clause after the function header
void CodeGenerator::InsertConceptConstraint(const FunctionDecl* tmplDecl)
{
    SmallVector<const Expr*, 5> constraints{};
    tmplDecl->getAssociatedConstraints(constraints);

    InsertConceptConstraint(constraints, InsertInline::Yes);
}
//-----------------------------------------------------------------------------

// This inserts the requires clause after a variable type
void CodeGenerator::InsertConceptConstraint(const VarDecl* varDecl)
{
    if(const auto* t = varDecl->getType()->getContainedAutoType()) {
        if(t->getTypeConstraintConcept()) {
#if 0
            mOutputFormatHelper.Append(kwCommentStart, t->getTypeConstraintConcept()->getName(), kwCCommentEndSpace);
#endif
        }
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertFunctionNameWithReturnType(const FunctionDecl&       decl,
                                                     const CXXConstructorDecl* cxxInheritedCtorDecl)
{
    bool        isLambda{false};
    bool        isFirstCxxMethodDecl{true};
    const auto* methodDecl{dyn_cast_or_null<CXXMethodDecl>(&decl)};
    bool        isCXXMethodDecl{nullptr != methodDecl};
    const bool  isClassTemplateSpec{isCXXMethodDecl and isa<ClassTemplateSpecializationDecl>(methodDecl->getParent())};
    const bool  requiresComment{isCXXMethodDecl and not methodDecl->isUserProvided() and
                               not methodDecl->isExplicitlyDefaulted()};
    // [expr.prim.lambda.closure] p7 consteval/constexpr are obtained from the call operator
    const bool          isLambdaStaticInvoker{isCXXMethodDecl and methodDecl->isLambdaStaticInvoker()};
    const FunctionDecl& constExprDecl{not isLambdaStaticInvoker ? decl
                                                                : *methodDecl->getParent()->getLambdaCallOperator()};
    const auto          desugaredReturnType = GetType(GetDesugarReturnType(decl));

    if(methodDecl) {
        if(requiresComment) {
            mOutputFormatHelper.Append(kwCppCommentStartSpace);
        }

        isLambda             = methodDecl->getParent()->isLambda();
        isFirstCxxMethodDecl = (nullptr == methodDecl->getPreviousDecl());
    }

    // types of conversion decls can be invalid to type at this place. So introduce a using
    if(isa<CXXConversionDecl>(decl) and TypeContainsSubType<PointerType, FunctionProtoType>(desugaredReturnType)) {
        mOutputFormatHelper.AppendSemiNewLine(
            kwUsingSpace, BuildRetTypeName(decl), hlpAssing, GetName(desugaredReturnType));
    }

    if(isCXXMethodDecl and decl.isOutOfLine()) {
        if(const auto* parent = methodDecl->getParent()) {
            if(const auto* outerClasTemplateDecl = parent->getDescribedClassTemplate()) {
                InsertTemplateParameters(*outerClasTemplateDecl->getTemplateParameters());
            }
        }
    }

    if(decl.isTemplated()) {
        if(decl.getDescribedTemplate()) {
            InsertTemplateParameters(*decl.getDescribedTemplate()->getTemplateParameters());
        }

    } else if(decl.isFunctionTemplateSpecialization() or (isClassTemplateSpec and decl.isOutOfLine() and
                                                          (decl.getLexicalDeclContext() != methodDecl->getParent()))) {
        InsertTemplateSpecializationHeader(decl);
    }

    InsertAttributes(decl.attrs());

    if(not decl.isFunctionTemplateSpecialization() or (isCXXMethodDecl and isFirstCxxMethodDecl)) {
        if(not decl.isOutOfLine() or (decl.getStorageClass() == SC_Extern)) {
            mOutputFormatHelper.Append(GetStorageClassAsStringWithSpace(decl.getStorageClass()));
        }

        // [class.free]: Any allocation function for a class T is a static member (even if not explicitly declared
        // static). (https://eel.is/c++draft/class.free#1)
        // However, Clang does not add `static` to `getStorageClass` so this needs to be check independently.
        if(isCXXMethodDecl and not decl.isOutOfLine()) {
            // GetStorageClassAsStringWithSpace already carries static, if the method was marked so explicitly
            if((not IsStaticStorageClass(methodDecl)) and (methodDecl->isStatic())) {
                mOutputFormatHelper.Append(kwStaticSpace);
            }
        }
    }

    if(Decl::FOK_None != decl.getFriendObjectKind()) {
        mOutputFormatHelper.Append(kwFriendSpace);
    }

    if(decl.isInlined()) {
        mOutputFormatHelper.Append(kwInlineSpace);
    }

    if(methodDecl and isFirstCxxMethodDecl) {
        if(methodDecl->isVirtual()) {
            mOutputFormatHelper.Append(kwVirtualSpace);
        }

        const auto exspec = ExplicitSpecifier::getFromDecl(methodDecl);

        if(const auto* expr = exspec.getExpr()) {
            mOutputFormatHelper.Append(kwExplicit);

            WrapInParens(
                [&] {
                    switch(exspec.getKind()) {
                        case ExplicitSpecKind::Unresolved: InsertArg(expr); break;
                        case ExplicitSpecKind::ResolvedFalse: mOutputFormatHelper.Append(kwFalse); break;
                        case ExplicitSpecKind::ResolvedTrue: mOutputFormatHelper.Append("true"sv); break;
                    }
                },
                AddSpaceAtTheEnd::Yes);

        } else if(exspec.isExplicit()) {
            mOutputFormatHelper.Append(kwExplicitSpace);
        }
    }

    if(constExprDecl.isConstexpr()) {
        const bool skipConstexpr{isLambda and not isa<CXXConversionDecl>(constExprDecl)};
        // Special treatment for a conversion operator in a captureless lambda. It appears that if the call operator
        // is consteval the conversion operator must be as well, otherwise it cannot take the address of the invoke
        // function.
        const bool isConversionOpWithConstevalCallOp{[&]() {
            if(methodDecl) {
                if(const auto callOp = methodDecl->getParent()->getLambdaCallOperator()) {
                    return callOp->isConsteval();
                }
            }

            return false;
        }()};

        if(not isConversionOpWithConstevalCallOp and constExprDecl.isConstexprSpecified()) {
            if(skipConstexpr) {
                mOutputFormatHelper.Append(kwCommentStart);
            }

            mOutputFormatHelper.Append(kwConstExprSpace);

            if(skipConstexpr) {
                mOutputFormatHelper.Append(kwCCommentEndSpace);
            }

        } else if(isConversionOpWithConstevalCallOp or constExprDecl.isConsteval()) {
            mOutputFormatHelper.Append(kwConstEvalSpace);
        }
    }

    // temporary output to be able to handle a return value of array reference
    OutputFormatHelper outputFormatHelper{};

    if(methodDecl) {
        if(not isFirstCxxMethodDecl or InsertNamespace() and decl.getQualifier()) {
            CodeGeneratorVariant cg{outputFormatHelper};
            cg->InsertNamespace(decl.getQualifier());

            // This comes from a using Base::SomeFunc
        } else if(not isFirstCxxMethodDecl or InsertNamespace() and not decl.getQualifier()) {
            const auto* parent = methodDecl->getParent();
            outputFormatHelper.Append(parent->getName(), "::"sv);
        }
    }

    if(not isa<CXXConversionDecl>(decl)) {
        if(isa<CXXConstructorDecl>(decl) or isa<CXXDestructorDecl>(decl)) {
            if(methodDecl) {
                if(isa<CXXDestructorDecl>(decl)) {
                    outputFormatHelper.Append('~');
                }

                outputFormatHelper.Append(GetName(*methodDecl->getParent()));
            }

        } else {
            outputFormatHelper.Append(GetName(decl));
        }

        if(isFirstCxxMethodDecl and decl.isFunctionTemplateSpecialization()) {
            CodeGeneratorVariant codeGenerator{outputFormatHelper};
            codeGenerator->InsertTemplateArgs(decl);
        }

        outputFormatHelper.Append('(');
    }

    // if a CXXInheritedCtorDecl was passed as a pointer us this to get the parameters from.
    if(cxxInheritedCtorDecl) {
        outputFormatHelper.AppendParameterList(cxxInheritedCtorDecl->parameters(),
                                               OutputFormatHelper::NameOnly::No,
                                               OutputFormatHelper::GenMissingParamName::Yes);
    } else {
        // The static invoker needs parameter names to forward parameters to the call operator even when the call
        // operator doesn't care about them.
        const OutputFormatHelper::GenMissingParamName genMissingParamName{
            isLambdaStaticInvoker ? OutputFormatHelper::GenMissingParamName::Yes
                                  : OutputFormatHelper::GenMissingParamName::No};

        outputFormatHelper.AppendParameterList(
            decl.parameters(), OutputFormatHelper::NameOnly::No, genMissingParamName);

        if(GetInsightsOptions().UseShow2C and not decl.isVariadic() and decl.param_empty()) {
            outputFormatHelper.Append("void"sv);
        }
    }

    if(decl.isVariadic()) {
        outputFormatHelper.Append(", ..."sv);
    }

    outputFormatHelper.Append(')');

    if(not isa<CXXConstructorDecl>(decl) and not isa<CXXDestructorDecl>(decl)) {
        if(isa<CXXConversionDecl>(decl)) {
            const std::string typeName{TypeContainsSubType<PointerType, FunctionProtoType>(desugaredReturnType)
                                           ? BuildRetTypeName(decl)
                                           : GetName(desugaredReturnType)};

            mOutputFormatHelper.Append(kwOperatorSpace, typeName, " ("sv, outputFormatHelper.GetString());
        } else {
            mOutputFormatHelper.Append(GetTypeNameAsParameter(desugaredReturnType, outputFormatHelper.GetString()));
        }
    } else {
        mOutputFormatHelper.Append(outputFormatHelper);
    }

    mOutputFormatHelper.Append(GetConst(decl));

    if(methodDecl) {
        if(methodDecl->isVolatile()) {
            mOutputFormatHelper.Append(kwSpaceVolatile);
        }

        if(methodDecl->hasAttr<FinalAttr>()) {
            mOutputFormatHelper.Append(kwSpaceFinal);
        }
    }

    switch(decl.getType()->getAs<FunctionProtoType>()->getRefQualifier()) {
        case RQ_None: break;
        case RQ_LValue: mOutputFormatHelper.Append(" &"sv); break;
        case RQ_RValue: mOutputFormatHelper.Append(" &&"sv); break;
    }

    mOutputFormatHelper.Append(GetNoExcept(decl));

    // insert the trailing requires-clause, if any. In case, this is a template then we already inserted the
    // template requires-clause during creation of the template head.
    InsertConceptConstraint(&decl);

    if(decl.isPureVirtual()) {
        mOutputFormatHelper.Append(" = 0"sv);
    }

    if(decl.isDeleted()) {
        mOutputFormatHelper.Append(kwSpaceEqualsDelete);
        if(auto* delInfo = decl.getDefalutedOrDeletedInfo()) {
            WrapInParens([&]() { InsertArg(delInfo->getDeletedMessage()); }, AddSpaceAtTheEnd::No);
        } else {
            mOutputFormatHelper.AppendSemiNewLine();
        }

    } else if(decl.isDefaulted()) {
        mOutputFormatHelper.AppendNewLine(kwSpaceEqualsDefault);
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertCurlysIfRequired(const Stmt* stmt)
{
    const bool requiresCurlys{not isa<InitListExpr>(stmt) and not isa<ParenExpr>(stmt) and
                              not isa<CXXDefaultInitExpr>(stmt)};

    if(requiresCurlys) {
        mOutputFormatHelper.Append('{');
    }

    InsertArg(stmt);

    if(requiresCurlys) {
        mOutputFormatHelper.Append('}');
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::WrapInParensOrCurlys(const BraceKind        braceKind,
                                         void_func_ref          lambda,
                                         const AddSpaceAtTheEnd addSpaceAtTheEnd)
{
    if(BraceKind::Curlys == braceKind) {
        mOutputFormatHelper.Append('{');
    } else {
        mOutputFormatHelper.Append('(');
    }

    lambda();

    if(BraceKind::Curlys == braceKind) {
        mOutputFormatHelper.Append('}');
    } else {
        mOutputFormatHelper.Append(')');
    }

    if(AddSpaceAtTheEnd::Yes == addSpaceAtTheEnd) {
        mOutputFormatHelper.Append(' ');
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::WrapInCompoundIfNeeded(const Stmt* stmt, const AddNewLineAfter addNewLineAfter)
{
    const bool hasNoCompoundStmt = not(isa<CompoundStmt>(stmt) or isa<AttributedStmt>(stmt));

    if(hasNoCompoundStmt) {
        mOutputFormatHelper.OpenScope();
    }

    if(not isa<NullStmt>(stmt)) {
        InsertArg(stmt);

        const bool isAttrWithCompound{[&] {
            auto* attrStmt = dyn_cast_or_null<AttributedStmt>(stmt);
            return attrStmt and isa<CompoundStmt>(attrStmt->getSubStmt());
        }()};

        // Add semi-colon if necessary. A do{} while does already add one.
        if(IsStmtRequiringSemi<IfStmt, CompoundStmt, NullStmt, WhileStmt, DoStmt>(stmt) and not isAttrWithCompound) {
            mOutputFormatHelper.AppendSemiNewLine();
        }
    }

    if(hasNoCompoundStmt) {
        mOutputFormatHelper.CloseScope(OutputFormatHelper::NoNewLineBefore::Yes);
    }

    const bool addNewLine = (AddNewLineAfter::Yes == addNewLineAfter);
    if(addNewLine or (hasNoCompoundStmt and addNewLine)) {
        mOutputFormatHelper.AppendNewLine();
    } else if(not addNewLine or (hasNoCompoundStmt and not addNewLine)) {
        mOutputFormatHelper.Append(' ');
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::WrapInParens(void_func_ref lambda, const AddSpaceAtTheEnd addSpaceAtTheEnd)
{
    WrapInParensOrCurlys(BraceKind::Parens, lambda, addSpaceAtTheEnd);
}
//-----------------------------------------------------------------------------

void CodeGenerator::WrapInParensIfNeeded(bool                   needsParens,
                                         void_func_ref          lambda,
                                         const AddSpaceAtTheEnd addSpaceAtTheEnd)
{
    if(needsParens) {
        WrapInParensOrCurlys(BraceKind::Parens, lambda, addSpaceAtTheEnd);
    } else {
        lambda();
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::WrapInCurliesIfNeeded(bool                   needsParens,
                                          void_func_ref          lambda,
                                          const AddSpaceAtTheEnd addSpaceAtTheEnd)
{
    if(needsParens) {
        WrapInParensOrCurlys(BraceKind::Curlys, lambda, addSpaceAtTheEnd);
    } else {
        lambda();
    }
}
//-----------------------------------------------------------------------------

void CodeGenerator::WrapInCurlys(void_func_ref lambda, const AddSpaceAtTheEnd addSpaceAtTheEnd)
{
    WrapInParensOrCurlys(BraceKind::Curlys, lambda, addSpaceAtTheEnd);
}
//-----------------------------------------------------------------------------

void CodeGenerator::InsertArg(const BindingDecl*)
{
    // We ignore this here in the global level. In some cases a BindingDecl appears _before_ the DecompositionDecl
    // which leads to invalid code. See StructuredBindingsHandler3Test.cpp.
}
//-----------------------------------------------------------------------------

void StructuredBindingsCodeGenerator::InsertArg(const BindingDecl* stmt)
{
    const auto* bindingStmt = stmt->getBinding();

    // In a dependent context we have no binding and with that no type. Leave this as it is, we are looking at a
    // primary template here.
    RETURN_IF(not bindingStmt);

    // Assume that we are looking at a builtin type. We have to construct the variable declaration information.
    auto type = stmt->getType();

    // If we have a holding var we are looking at a user defined type like tuple and those the defaults from above
    // are wrong. This type contains the variable declaration so we insert this.
    if(const auto* holdingVar = stmt->getHoldingVar()) {
        // Initial paper: http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2015/p0144r0.pdf

        // The type of the binding depends on the initializer. In case the initializer is an lvalue we get a T&,
        // otherwise a T&&. We typically look at an lvalue if the decomposition declaration was auto& [a,b]. Note
        // the & here We have a rvalue in case the decomposition declaration was auto [a,b]. Note no reference. The
        // standard std::get returns a lvalue reference in case e in get(e) is an lvalue, otherwise it returns an
        // rvalue reference because then the call is get(std::move(e))
        type = holdingVar->getType().getCanonicalType();

        bindingStmt = holdingVar->getAnyInitializer();

    } else if(not type->isLValueReferenceType()) {
        type = stmt->getASTContext().getLValueReferenceType(type);
    }

    InsertAttributes(stmt->attrs());

    mOutputFormatHelper.Append(GetQualifiers(*dyn_cast_or_null<VarDecl>(stmt->getDecomposedDecl())),
                               GetTypeNameAsParameter(type, GetName(*stmt)),
                               hlpAssing);

    InsertArg(bindingStmt);

    mOutputFormatHelper.AppendSemiNewLine();
}
//-----------------------------------------------------------------------------

void StructuredBindingsCodeGenerator::InsertDecompositionBindings(const DecompositionDecl& decompositionDeclStmt)
{
    for(const auto* bindingDecl : decompositionDeclStmt.bindings()) {
        InsertArg(bindingDecl);
    }
}
//-----------------------------------------------------------------------------

void StructuredBindingsCodeGenerator::InsertArg(const DeclRefExpr* stmt)
{
    const auto name = GetName(*stmt);

    mOutputFormatHelper.Append(name);

    if(name.empty()) {
        mOutputFormatHelper.Append(mVarName);
    } else {
        InsertTemplateArgs(*stmt);
    }
}
//-----------------------------------------------------------------------------

void LambdaCodeGenerator::InsertArg(const CXXThisExpr* stmt)
{
    DPrint("thisExpr: imlicit=%d %s\n", stmt->isImplicit(), GetName(GetDesugarType(stmt->getType())));

    if(mCapturedThisAsCopy) {
        mOutputFormatHelper.Append("(&"sv, kwInternalThis, ")"sv);

    } else {
        mOutputFormatHelper.Append(kwInternalThis);
    }
}
//-----------------------------------------------------------------------------

}  // namespace clang::insights
