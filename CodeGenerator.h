/******************************************************************************
 *
 * C++ Insights, copyright (C) by Andreas Fertig
 * Distributed under an MIT license. See LICENSE for details
 *
 ****************************************************************************/

#ifndef INSIGHTS_CODE_GENERATOR_H
#define INSIGHTS_CODE_GENERATOR_H

#include "clang/AST/ASTContext.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/ADT/APInt.h"

#include <optional>

#include "ClangCompat.h"
#include "InsightsStaticStrings.h"
#include "InsightsStrongTypes.h"
#include "InsightsUtility.h"
#include "OutputFormatHelper.h"
#include "StackList.h"
//-----------------------------------------------------------------------------

namespace clang::insights {

void PushVtableEntry(const CXXRecordDecl*, const CXXRecordDecl*, VarDecl* decl);
int  GetGlobalVtablePos(const CXXRecordDecl*, const CXXRecordDecl*);

class CppInsightsCommentStmt : public Stmt
{
    std::string mComment{};

public:
    CppInsightsCommentStmt(std::string_view comment)
    : Stmt{NoStmtClass}
    , mComment{comment}
    {
    }

    std::string_view Comment() const { return mComment; }

    static bool classof(const Stmt* T) { return T->getStmtClass() == NoStmtClass; }

    child_range children()
    {
        static child_iterator iter{};
        return {iter, iter};
    }
    const_child_range children() const
    {
        static const_child_iterator iter{};
        return {iter, iter};
    }
};

struct LifetimeEntry
{
    STRONG_BOOL(FuncStart);

    const VarDecl* item{};
    FuncStart      funcStart{FuncStart::No};
    int            scope{};
};

class LifetimeTracker
{
    inline static int scopeCounter{};

    SmallVector<LifetimeEntry, 10> objects{};

    void InsertDtorCall(const VarDecl* decl, OutputFormatHelper& ofm);

public:
    void Add(const VarDecl* decl);
    void AddExtended(const VarDecl* decl, const ValueDecl* extending);

    LifetimeEntry& top() { return objects.back(); }

    void removeTop();
    void StartScope(bool funcStart);
    bool Return(OutputFormatHelper& ofm);
    bool EndScope(OutputFormatHelper& ofm, bool clear);
};

/// \brief More or less the heart of C++ Insights.
///
/// This is the place where nearly all of the transformations happen. This class knows the needed types and how to
/// generated code from them.
class CodeGenerator
{
protected:
    LifetimeTracker mLifeTimeTracker{};
    const Stmt*     mLastStmt{};
    const Expr*     mLastExpr{};  // special case for assignments to class member

public:
    const Decl* mLastDecl{};

protected:
    bool mProcessingVarDecl{};
    friend class CodeGeneratorVariant;

    OutputFormatHelper& mOutputFormatHelper;

    enum class LambdaCallerType
    {
        VarDecl,
        InitCapture,
        CallExpr,
        OperatorCallExpr,
        MemberCallExpr,
        LambdaExpr,
        ReturnStmt,
        BinaryOperator,
        CXXMethodDecl,
        TemplateHead,
        Decltype,
    };

    class LambdaHelper : public StackListEntry<LambdaHelper>
    {
    public:
        LambdaHelper(const LambdaCallerType lambdaCallerType, OutputFormatHelper& outputFormatHelper)
        : mLambdaCallerType{lambdaCallerType}
        , mCurrentVarDeclPos{outputFormatHelper.CurrentPos()}
        , mOutputFormatHelper{outputFormatHelper}
        {
            mLambdaOutputFormatHelper.SetIndent(mOutputFormatHelper);
        }

        void finish()
        {
            if(not mLambdaOutputFormatHelper.empty()) {
                mOutputFormatHelper.InsertAt(mCurrentVarDeclPos, mLambdaOutputFormatHelper);
            }
        }

        OutputFormatHelper& buffer() { return mLambdaOutputFormatHelper; }

        std::string& inits() { return mInits; }

        void insertInits(OutputFormatHelper& outputFormatHelper)
        {
            if(not mInits.empty()) {
                outputFormatHelper.Append(mInits);
                mInits.clear();
            }
        }

        LambdaCallerType callerType() const { return mLambdaCallerType; }
        bool             insertName() const { return (LambdaCallerType::Decltype != mLambdaCallerType) or mForceName; }

        void setInsertName(bool b) { mForceName = b; }

    private:
        const LambdaCallerType mLambdaCallerType;
        const size_t           mCurrentVarDeclPos;
        OutputFormatHelper&    mOutputFormatHelper;
        OutputFormatHelper     mLambdaOutputFormatHelper{};
        std::string            mInits{};
        bool                   mForceName{};
    };
    //-----------------------------------------------------------------------------

    using LambdaStackType = StackList<class LambdaHelper>;

    STRONG_BOOL(LambdaInInitCapture);  ///! Signal whether we are processing a lambda created and assigned to an init
                                       /// capture of another lambda.

    STRONG_BOOL(
        ProcessingPrimaryTemplate);  ///! We do not want to transform a primary template which contains a Coroutine.

    constexpr CodeGenerator(OutputFormatHelper&       _outputFormatHelper,
                            LambdaStackType&          lambdaStack,
                            LambdaInInitCapture       lambdaInitCapture,
                            ProcessingPrimaryTemplate processingPrimaryTemplate)
    : mOutputFormatHelper{_outputFormatHelper}
    , mLambdaStack{lambdaStack}
    , mLambdaInitCapture{lambdaInitCapture}
    , mProcessingPrimaryTemplate{processingPrimaryTemplate}
    {
    }

public:
    explicit constexpr CodeGenerator(OutputFormatHelper& _outputFormatHelper)
    : CodeGenerator{_outputFormatHelper, mLambdaStackThis, ProcessingPrimaryTemplate::No}
    {
    }

    constexpr CodeGenerator(OutputFormatHelper& _outputFormatHelper, LambdaInInitCapture lambdaInitCapture)
    : CodeGenerator{_outputFormatHelper, mLambdaStackThis, lambdaInitCapture, ProcessingPrimaryTemplate::No}
    {
    }

    constexpr CodeGenerator(OutputFormatHelper&       _outputFormatHelper,
                            LambdaStackType&          lambdaStack,
                            ProcessingPrimaryTemplate processingPrimaryTemplate)
    : CodeGenerator{_outputFormatHelper, lambdaStack, LambdaInInitCapture::No, processingPrimaryTemplate}
    {
    }

    virtual ~CodeGenerator() = default;

#define IGNORED_DECL(type)                                                                                             \
    virtual void InsertArg(const type*) {}
#define IGNORED_STMT(type)                                                                                             \
    virtual void InsertArg(const type*) {}
#define SUPPORTED_DECL(type) virtual void InsertArg(const type* stmt);
#define SUPPORTED_STMT(type) virtual void InsertArg(const type* stmt);

#include "CodeGeneratorTypes.h"

    virtual void InsertArg(const Decl* stmt);
    virtual void InsertArg(const Stmt* stmt);

    template<typename T>
    void InsertTemplateArgs(const T& t)
    {
        if constexpr(std::is_same_v<T, FunctionDecl>) {
            if(const auto* tmplArgs = t.getTemplateSpecializationArgs()) {
                InsertTemplateArgs(*tmplArgs);
            }
        } else if constexpr(std::is_same_v<T, VarTemplateSpecializationDecl>) {
            InsertTemplateArgs(t.getTemplateArgs());

        } else if constexpr(requires { t.template_arguments(); }) {
            if constexpr(std::is_same_v<DeclRefExpr, T>) {
                if(0 == t.getNumTemplateArgs()) {
                    return;
                }
            }

            InsertTemplateArgs(t.template_arguments());

        } else if constexpr(requires { t.asArray(); }) {
            InsertTemplateArgs(t.asArray());
        }
    }

    void InsertTemplateArgs(const ClassTemplateSpecializationDecl& clsTemplateSpe);

    /// \brief Insert the code for a FunctionDecl.
    ///
    /// This inserts the code of a FunctionDecl (and everything which is derived from one). It takes care of
    /// CXXMethodDecl's access modifier as well as things like constexpr, noexcept, static and more.
    ///
    /// @param decl The FunctionDecl to process.
    /// @param skipAccess Show or hide access modifiers (public, private, protected). The default is to show them.
    /// @param cxxInheritedCtorDecl If not null, the type and name of this decl is used for the parameters.
    void InsertFunctionNameWithReturnType(const FunctionDecl&       decl,
                                          const CXXConstructorDecl* cxxInheritedCtorDecl = nullptr);

    template<typename T>
    void InsertTemplateArgs(const ArrayRef<T>& array)
    {
        mOutputFormatHelper.Append('<');

        ForEachArg(array, [&](const auto& arg) { InsertTemplateArg(arg); });

        /* put as space between to closing brackets: >> -> > > */
        if(mOutputFormatHelper.GetString().back() == '>') {
            mOutputFormatHelper.Append(' ');
        }

        mOutputFormatHelper.Append('>');
    }

    void InsertAttributes(const Decl*);
    void InsertAttributes(const Decl::attr_range&);
    void InsertAttribute(const Attr&);

    void InsertTemplateArg(const TemplateArgument& arg);

    STRONG_BOOL(TemplateParamsOnly);  ///! Skip template, type constraints and class/typename.
    void InsertTemplateParameters(const TemplateParameterList& list,
                                  const TemplateParamsOnly     templateParamsOnly = TemplateParamsOnly::No);

    void StartLifetimeScope();
    void LifetimeAddExtended(const VarDecl*, const ValueDecl*);
    void EndLifetimeScope();

protected:
    virtual bool InsertVarDecl(const VarDecl*) { return true; }
    virtual bool SkipSpaceAfterVarDecl() { return false; }
    virtual bool InsertComma() { return false; }
    virtual bool InsertSemi() { return true; }
    virtual bool InsertNamespace() const { return false; }

    /// \brief Show casts to xvalues independent from the show all casts option.
    ///
    /// This helps showing xvalue casts in structured bindings.
    virtual bool ShowXValueCasts() const { return false; }

    void HandleTemplateParameterPack(const ArrayRef<TemplateArgument>& args);
    void HandleCompoundStmt(const CompoundStmt* stmt);
    /// \brief Show what is behind a local static variable.
    ///
    /// [stmt.dcl] p4: Initialization of a block-scope variable with static storage duration is thread-safe since C++11.
    /// Regardless of that, as long as it is a non-trivally construct and destructable class the compiler adds code to
    /// track the initialization state. Reference:
    /// - www.opensource.apple.com/source/libcppabi/libcppabi-14/src/cxa_guard.cxx
    void HandleLocalStaticNonTrivialClass(const VarDecl* stmt);

    virtual void FormatCast(const std::string_view castName,
                            const QualType&        CastDestType,
                            const Expr*            SubExpr,
                            const CastKind&        castKind);

    void ForEachArg(const auto& arguments, auto&& lambda) { mOutputFormatHelper.ForEachArg(arguments, lambda); }

    void InsertArgWithParensIfNeeded(const Stmt* stmt);
    void InsertSuffix(const QualType& type);

    void InsertTemplateArg(const TemplateArgumentLoc& arg) { InsertTemplateArg(arg.getArgument()); }
    bool InsertLambdaStaticInvoker(const CXXMethodDecl* cxxMethodDecl);

    STRONG_BOOL(InsertInline);

    void InsertConceptConstraint(const llvm::SmallVectorImpl<const Expr*>& constraints,
                                 const InsertInline                        insertInline);
    void InsertConceptConstraint(const FunctionDecl* tmplDecl);
    void InsertConceptConstraint(const VarDecl* varDecl);
    void InsertConceptConstraint(const TemplateParameterList& tmplDecl);

    void InsertTemplate(const FunctionTemplateDecl*, bool withSpec);

    void InsertQualifierAndNameWithTemplateArgs(const DeclarationName& declName, const auto* stmt)
    {
        InsertQualifierAndName(declName, stmt->getQualifier(), stmt->hasTemplateKeyword());

        if(stmt->getNumTemplateArgs()) {
            InsertTemplateArgs(*stmt);
        } else if(stmt->hasExplicitTemplateArgs()) {
            // we have empty templates arguments, but angle brackets provided by the user
            mOutputFormatHelper.Append("<>"sv);
        }
    }

    void InsertQualifierAndName(const DeclarationName&     declName,
                                const NestedNameSpecifier* qualifier,
                                const bool                 hasTemplateKeyword);

    /// For a special case, when a LambdaExpr occurs in a Constructor from an
    /// in class initializer, there is a need for a more narrow scope for the \c LAMBDA_SCOPE_HELPER.
    void InsertCXXMethodHeader(const CXXMethodDecl* stmt, OutputFormatHelper& initOutputFormatHelper);

    void InsertTemplateGuardBegin(const FunctionDecl* stmt);
    void InsertTemplateGuardEnd(const FunctionDecl* stmt);

    /// \brief Insert \c template<> to introduce a template specialization.
    void InsertTemplateSpecializationHeader(const Decl&);

    void InsertTemplateArgsObjectParam(const ArrayRef<TemplateArgument>& array);
    void InsertTemplateArgsObjectParam(const TemplateParamObjectDecl& param);

    void InsertNamespace(const NestedNameSpecifier* namespaceSpecifier);
    void ParseDeclContext(const DeclContext* Ctx);

    STRONG_BOOL(SkipBody);
    virtual void InsertCXXMethodDecl(const CXXMethodDecl* stmt, SkipBody skipBody);
    void         InsertMethodBody(const FunctionDecl* stmt, const size_t posBeforeFunc);

    /// \brief Generalized function to insert either a \c CXXConstructExpr or \c CXXUnresolvedConstructExpr
    template<typename T>
    void InsertConstructorExpr(const T* stmt);

    /// \brief Check whether or not this statement will add curlys or parentheses and add them only if required.
    void InsertCurlysIfRequired(const Stmt* stmt);

    void InsertIfOrSwitchInitVariables(same_as_any_of<const IfStmt, const SwitchStmt> auto* stmt);

    void InsertInstantiationPoint(const SourceManager& sm, const SourceLocation& instLoc, std::string_view text = {});

    STRONG_BOOL(AddNewLineAfter);

    void WrapInCompoundIfNeeded(const Stmt* stmt, const AddNewLineAfter addNewLineAfter);

    STRONG_BOOL(AddSpaceAtTheEnd);

    enum class BraceKind
    {
        Parens,
        Curlys
    };

    void WrapInParens(void_func_ref lambda, const AddSpaceAtTheEnd addSpaceAtTheEnd = AddSpaceAtTheEnd::No);

    void WrapInParensIfNeeded(bool                   needsParens,
                              void_func_ref          lambda,
                              const AddSpaceAtTheEnd addSpaceAtTheEnd = AddSpaceAtTheEnd::No);

    void WrapInCurliesIfNeeded(bool                   needsParens,
                               void_func_ref          lambda,
                               const AddSpaceAtTheEnd addSpaceAtTheEnd = AddSpaceAtTheEnd::No);

    void WrapInCurlys(void_func_ref lambda, const AddSpaceAtTheEnd addSpaceAtTheEnd = AddSpaceAtTheEnd::No);

    void WrapInParensOrCurlys(const BraceKind        curlys,
                              void_func_ref          lambda,
                              const AddSpaceAtTheEnd addSpaceAtTheEnd = AddSpaceAtTheEnd::No);

    void UpdateCurrentPos(std::optional<size_t>& pos) { pos = mOutputFormatHelper.CurrentPos(); }

    static std::string_view GetBuiltinTypeSuffix(const BuiltinType::Kind& kind);

    class LambdaScopeHandler
    {
    public:
        LambdaScopeHandler(LambdaStackType&       stack,
                           OutputFormatHelper&    outputFormatHelper,
                           const LambdaCallerType lambdaCallerType);

        ~LambdaScopeHandler();

    private:
        LambdaStackType& mStack;
        LambdaHelper     mHelper;

        OutputFormatHelper& GetBuffer(OutputFormatHelper& outputFormatHelper) const;
    };

    void               HandleLambdaExpr(const LambdaExpr* stmt, LambdaHelper& lambdaHelper);
    static std::string FillConstantArray(const ConstantArrayType* ct, const std::string& value, const uint64_t startAt);
    static std::string GetValueOfValueInit(const QualType& t);

    bool InsideDecltype() const;

    LambdaStackType  mLambdaStackThis;
    LambdaStackType& mLambdaStack;

    STRONG_BOOL(SkipVarDecl);
    STRONG_BOOL(UseCommaInsteadOfSemi);
    STRONG_BOOL(NoEmptyInitList);
    STRONG_BOOL(ShowConstantExprValue);
    LambdaInInitCapture mLambdaInitCapture{LambdaInInitCapture::No};

    ShowConstantExprValue mShowConstantExprValue{ShowConstantExprValue::No};
    SkipVarDecl           mSkipVarDecl{SkipVarDecl::No};
    UseCommaInsteadOfSemi mUseCommaInsteadOfSemi{UseCommaInsteadOfSemi::No};
    NoEmptyInitList       mNoEmptyInitList{
        NoEmptyInitList::No};  //!< At least in case if a requires-clause containing T{} we don't want to get T{{}}.
    const LambdaExpr*     mLambdaExpr{};
    static constexpr auto MAX_FILL_VALUES_FOR_ARRAYS{
        uint64_t{100}};  //!< This is the upper limit of elements which will be shown for an array when filled by \c
                         //!< FillConstantArray.
    std::optional<size_t> mCurrentVarDeclPos{};   //!< The position in mOutputFormatHelper where a potential
                                                  //!< std::initializer_list expansion must be inserted.
    std::optional<size_t> mCurrentCallExprPos{};  //!< The position in mOutputFormatHelper where a potential
                                                  //!< std::initializer_list expansion must be inserted.
    std::optional<size_t> mCurrentReturnPos{};    //!< The position in mOutputFormatHelper from a return where a
                                                  //!< potential std::initializer_list expansion must be inserted.
    std::optional<size_t> mCurrentFieldPos{};     //!< The position in mOutputFormatHelper in a class where where a
                                                  //!< potential std::initializer_list expansion must be inserted.
    OutputFormatHelper* mOutputFormatHelperOutside{
        nullptr};                        //!< Helper output buffer for std::initializer_list expansion.
    bool mRequiresImplicitReturnZero{};  //!< Track whether this is a function with an imlpicit return 0.
    bool mSkipSemi{};
    ProcessingPrimaryTemplate                 mProcessingPrimaryTemplate{};
    static inline std::map<std::string, bool> mSeenDecls{};
};
//-----------------------------------------------------------------------------

class LambdaCodeGenerator final : public CodeGenerator
{
public:
    using CodeGenerator::CodeGenerator;

    using CodeGenerator::InsertArg;
    void InsertArg(const CXXThisExpr* stmt) override;

    bool mCapturedThisAsCopy{};
};
//-----------------------------------------------------------------------------

/*
 * \brief Special case to generate the inits of e.g. a \c ForStmt.
 *
 * This class is a specialization to handle cases where we can have multiple init statements to the same variable and
 * hence need only one time the \c VarDecl. An example a for-loops:
\code
for(int x=2, y=3, z=4; i < x; ++i) {}
\endcode
 */
class MultiStmtDeclCodeGenerator final : public CodeGenerator
{
public:
    explicit MultiStmtDeclCodeGenerator(OutputFormatHelper& _outputFormatHelper,
                                        LambdaStackType&    lambdaStack,
                                        bool                insertVarDecl)
    : CodeGenerator{_outputFormatHelper, lambdaStack, ProcessingPrimaryTemplate::No}
    , mInsertVarDecl{insertVarDecl}
    , mInsertComma{}
    {
    }

    // Insert the semi after the last declaration. This implies that this class always requires its own scope.
    ~MultiStmtDeclCodeGenerator() { mOutputFormatHelper.Append("; "sv); }

    using CodeGenerator::InsertArg;

protected:
    OnceTrue  mInsertVarDecl{};  //! Insert the \c VarDecl only once.
    OnceFalse mInsertComma{};    //! Insert the comma after we have generated the first \c VarDecl and we are about to
                                 //! insert another one.

    bool InsertVarDecl(const VarDecl*) override { return mInsertVarDecl; }
    bool InsertComma() override { return mInsertComma; }
    bool InsertSemi() override { return false; }
};
//-----------------------------------------------------------------------------

struct CoroutineASTData
{
    CXXRecordDecl*                  mFrameType{};
    FieldDecl*                      mResumeFnField{};
    FieldDecl*                      mDestroyFnField{};
    FieldDecl*                      mPromiseField{};
    FieldDecl*                      mSuspendIndexField{};
    FieldDecl*                      mInitialAwaitResumeCalledField{};
    MemberExpr*                     mInitialAwaitResumeCalledAccess{};
    DeclRefExpr*                    mFrameAccessDeclRef{};
    MemberExpr*                     mSuspendIndexAccess{};
    bool                            mDoInsertInDtor{};
    std::vector<const CXXThisExpr*> mThisExprs{};
};

///
/// \brief A special generator for coroutines. It is only activated, if \c -show-coroutines-transformation is given as a
/// command line option.
class CoroutinesCodeGenerator final : public CodeGenerator
{
public:
    explicit CoroutinesCodeGenerator(OutputFormatHelper& _outputFormatHelper, const size_t posBeforeFunc)
    : CoroutinesCodeGenerator{_outputFormatHelper, posBeforeFunc, {}, {}, {}}
    {
    }

    explicit CoroutinesCodeGenerator(OutputFormatHelper& _outputFormatHelper,
                                     const size_t        posBeforeFunc,
                                     std::string_view    fsmName,
                                     size_t              suspendsCount,
                                     CoroutineASTData    data)
    : CodeGenerator{_outputFormatHelper}
    , mPosBeforeFunc{posBeforeFunc}
    , mSuspendsCount{suspendsCount}
    , mFSMName{fsmName}
    , mASTData{data}
    {
    }

    ~CoroutinesCodeGenerator() override;

    using CodeGenerator::InsertArg;

    void InsertArg(const ImplicitCastExpr* stmt) override;
    void InsertArg(const CallExpr* stmt) override;
    void InsertArg(const CXXRecordDecl* stmt) override;
    void InsertArg(const OpaqueValueExpr* stmt) override;

    void InsertArg(const CoroutineBodyStmt* stmt) override;
    void InsertArg(const CoroutineSuspendExpr* stmt) override;
    void InsertArg(const CoreturnStmt* stmt) override;

    void InsertCoroutine(const FunctionDecl& fd, const CoroutineBodyStmt* body);

    std::string GetFrameName() const { return mFrameName; }

protected:
    bool InsertVarDecl(const VarDecl* vd) override { return mInsertVarDecl or (vd and vd->isStaticLocal()); }
    bool SkipSpaceAfterVarDecl() override { return not mInsertVarDecl; }

private:
    enum class eState
    {
        Invalid,
        InitialSuspend,
        Body,
        FinalSuspend,
    };

    eState                            mState{};
    const size_t                      mPosBeforeFunc;
    size_t                            mPosBeforeSuspendExpr{};
    size_t                            mSuspendsCount{};
    size_t                            mSuspendsCounter{};
    bool                              mInsertVarDecl{true};
    bool                              mSupressCasts{};
    bool                              mSupressRecordDecls{};
    std::string                       mFrameName{};
    std::string                       mFSMName{};
    CoroutineASTData                  mASTData{};
    llvm::DenseMap<const Stmt*, bool> mBinaryExprs{};
    static inline llvm::DenseMap<const Expr*, std::pair<const DeclRefExpr*, std::string>>
        mOpaqueValues{};  ///! Keeps track of the current set of opaque value

    QualType GetFrameType() const { return QualType(mASTData.mFrameType->getTypeForDecl(), 0); }
    QualType GetFramePointerType() const;

    std::string BuildResumeLabelName(int) const;
    FieldDecl*  AddField(std::string_view name, QualType type);

    void InsertArgWithNull(const Stmt* stmt);
};
//-----------------------------------------------------------------------------

///
/// \brief A special generator for coroutines. It is only activated, if \c -show-coroutines-transformation is given as a
/// command line option.
class CfrontCodeGenerator final : public CodeGenerator
{
    ///! A mapping for the pair method decl - derived-to-base-class to index in the vtable.
    static inline llvm::DenseMap<std::pair<const Decl*, std::pair<const CXXRecordDecl*, const CXXRecordDecl*>>, int>
         mVirtualFunctions{};
    bool mInsertSemi{true};  // We need to for int* p = new{5};

public:
    using CodeGenerator::CodeGenerator;

    using CodeGenerator::InsertArg;

    void InsertArg(const CXXThisExpr*) override;
    void InsertArg(const CXXDeleteExpr*) override;
    void InsertArg(const CXXNewExpr*) override;
    void InsertArg(const CXXOperatorCallExpr*) override;
    void InsertArg(const CXXNullPtrLiteralExpr*) override;
    void InsertArg(const StaticAssertDecl*) override;
    void InsertArg(const CXXRecordDecl*) override;
    void InsertArg(const CXXMemberCallExpr*) override;
    void InsertArg(const CXXConstructExpr*) override;
    void InsertArg(const FunctionDecl* stmt) override;
    void InsertArg(const TypedefDecl* stmt) override;

    void InsertCXXMethodDecl(const CXXMethodDecl*, CodeGenerator::SkipBody) override;

    void FormatCast(const std::string_view, const QualType&, const Expr*, const CastKind&) override;

    struct CfrontVtableData
    {
        CfrontVtableData();

        // struct __mptr *__ptbl_vec__c___src_C_[]
        VarDecl*   VtblArrayVar(int size);
        FieldDecl* VtblPtrField(const CXXRecordDecl* parent);

        QualType vptpTypedef;  // typedef int (*__vptp)();

        /*
struct __mptr
{
    short  d;
    short  i;
    __vptp f;
};
*/
        CXXRecordDecl* vtableRecorDecl;
        QualType       vtableRecordType;
        FieldDecl*     d;
        FieldDecl*     f;
    };

    static CfrontVtableData& VtableData();

protected:
    bool InsertSemi() override { return std::exchange(mInsertSemi, true); }
};
//-----------------------------------------------------------------------------

///
/// \brief A special container which creates either a \c CodeGenerator or a \c CfrontCodeGenerator depending on the
/// command line options.
class CodeGeneratorVariant
{
    union CodeGenerators
    {
        CodeGenerator       cg;
        CfrontCodeGenerator cfcg;

        CodeGenerators(OutputFormatHelper&                      _outputFormatHelper,
                       CodeGenerator::LambdaStackType&          lambdaStack,
                       CodeGenerator::ProcessingPrimaryTemplate processingPrimaryTemplate);
        CodeGenerators(OutputFormatHelper& _outputFormatHelper, CodeGenerator::LambdaInInitCapture lambdaInitCapture);

        ~CodeGenerators();
    } cgs;

    CodeGenerator*      cg;
    OutputFormatHelper& ofm;

    void Set();

public:
    CodeGeneratorVariant(OutputFormatHelper& _outputFormatHelper)
    : CodeGeneratorVariant{_outputFormatHelper, CodeGenerator::LambdaInInitCapture::No}
    {
    }

    CodeGeneratorVariant(OutputFormatHelper&                      _outputFormatHelper,
                         CodeGenerator::LambdaStackType&          lambdaStack,
                         CodeGenerator::ProcessingPrimaryTemplate processingPrimaryTemplate)
    : cgs{_outputFormatHelper, lambdaStack, processingPrimaryTemplate}
    , ofm{_outputFormatHelper}
    , cg{}
    {
        Set();
    }

    CodeGeneratorVariant(OutputFormatHelper& _outputFormatHelper, CodeGenerator::LambdaInInitCapture lambdaInitCapture)
    : cgs{_outputFormatHelper, lambdaInitCapture}
    , ofm{_outputFormatHelper}
    , cg{}
    {
        Set();
    }

    CodeGenerator* operator->() { return cg; }
};
//-----------------------------------------------------------------------------

}  // namespace clang::insights

#endif /* INSIGHTS_CODE_GENERATOR_H */
