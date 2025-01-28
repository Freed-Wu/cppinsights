/******************************************************************************
 *
 * C++ Insights, copyright (C) by Andreas Fertig
 * Distributed under an MIT license. See LICENSE for details
 *
 ****************************************************************************/

#ifndef INSIGHTS_HELPERS_H
#define INSIGHTS_HELPERS_H
//-----------------------------------------------------------------------------

#include "clang/AST/AST.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Lex/Lexer.h"

#include <functional>
#include <optional>
#include <string>
#include <variant>

#include "InsightsStrongTypes.h"
#include "StackList.h"
//-----------------------------------------------------------------------------

namespace clang::insights {

std::string BuildTemplateParamObjectName(std::string name);
//-----------------------------------------------------------------------------

std::string BuildInternalVarName(const std::string_view& varName);
//-----------------------------------------------------------------------------

std::string MakeLineColumnName(const SourceManager& sm, const SourceLocation& loc, const std::string_view& prefix);
//-----------------------------------------------------------------------------

inline bool IsStaticStorageClass(const CXXMethodDecl* md)
{
    return SC_Static == md->getStorageClass();
}
//-----------------------------------------------------------------------------

inline bool IsReferenceType(const ValueDecl* decl)
{
    return decl and decl->getType()->isReferenceType();
}
//-----------------------------------------------------------------------------

inline bool IsReferenceType(const DeclRefExpr* decl)
{
    return decl and IsReferenceType(decl->getDecl());
}
//-----------------------------------------------------------------------------

std::string BuildRetTypeName(const Decl& decl);
//-----------------------------------------------------------------------------

inline bool Contains(const std::string_view source, const std::string_view search)
{
    return std::string::npos != source.find(search, 0);
}
//-----------------------------------------------------------------------------

template<typename K, typename V, typename U>
inline bool Contains(const llvm::DenseMap<K, V>& map, const U& key)
{
    return map.find(key) != map.end();
}
//-----------------------------------------------------------------------------

void ReplaceAll(std::string& str, std::string_view from, std::string_view to);
//-----------------------------------------------------------------------------

void InsertBefore(std::string& source, const std::string_view& find, const std::string_view& replace);
//-----------------------------------------------------------------------------

inline const SourceManager& GetSM(const Decl& decl)
{
    return decl.getASTContext().getSourceManager();
}
//-----------------------------------------------------------------------------

inline const LangOptions& GetLangOpts(const Decl& decl)
{
    return decl.getASTContext().getLangOpts();
}
//-----------------------------------------------------------------------------

/// \brief Get the evaluated APValue from a `VarDecl`
///
/// Returns `nullptr` is the \c VarDecl is not evaluatable.
APValue* GetEvaluatedValue(const VarDecl& varDecl);
//-----------------------------------------------------------------------------

/// \brief Check whether a `VarDecl`s initialization can be done a compile-time.
///
/// This method checks, whether a \c VarDecl is initialized by a constant expression.
bool IsEvaluatable(const VarDecl& varDecl);
//-----------------------------------------------------------------------------

bool IsTrivialStaticClassVarDecl(const VarDecl& varDecl);
//-----------------------------------------------------------------------------

/*
 * Get the name of a DeclRefExpr without the namespace
 */
std::string GetPlainName(const DeclRefExpr& DRE);

std::string GetName(const DeclRefExpr& declRefExpr);
std::string GetName(const VarDecl& VD);
std::string GetName(const TemplateParamObjectDecl& decl);
//-----------------------------------------------------------------------------

STRONG_BOOL(QualifiedName);
std::string GetName(const NamedDecl& nd, const QualifiedName qualifiedName = QualifiedName::No);
//-----------------------------------------------------------------------------

std::string GetNameAsFunctionPointer(const QualType& t);
//-----------------------------------------------------------------------------

std::string GetLambdaName(const CXXRecordDecl& lambda);

inline std::string GetLambdaName(const LambdaExpr& lambda)
{
    return GetLambdaName(*lambda.getLambdaClass());
}
//-----------------------------------------------------------------------------

std::string GetName(const CXXRecordDecl& RD);
//-----------------------------------------------------------------------------

std::string GetName(const CXXTemporaryObjectExpr& tmp);
//-----------------------------------------------------------------------------

std::string GetTemporaryName(const Expr& tmp);
//-----------------------------------------------------------------------------

/// \brief In Cfront mode we transform references to pointers
QualType GetType(QualType);
//-----------------------------------------------------------------------------

/// \brief Check whether this is an anonymous struct or union.
///
/// There is a dedicated function `isAnonymousStructOrUnion` which at this point no longer returns true. Hence this
/// method uses an empty record decl name as indication for an anonymous struct/union.
inline bool IsAnonymousStructOrUnion(const CXXRecordDecl* cxxRecordDecl)
{
    if(cxxRecordDecl) {
        return cxxRecordDecl->getName().empty();
    }

    return false;
}
//-----------------------------------------------------------------------------

void AppendTemplateTypeParamName(class OutputFormatHelper&   ofm,
                                 const TemplateTypeParmDecl* decl,
                                 const bool                  isParameter,
                                 const TemplateTypeParmType* type = nullptr);
//-----------------------------------------------------------------------------

/// \brief Remove decltype from a QualType, if possible.
const QualType GetDesugarType(const QualType& QT);
// -----------------------------------------------------------------------------

inline QualType GetDesugarReturnType(const FunctionDecl& FD)
{
    return GetDesugarType(FD.getReturnType());
}
//-----------------------------------------------------------------------------

STRONG_BOOL(Unqualified);

std::string GetName(const QualType& t, const Unqualified unqualified = Unqualified::No);
//-----------------------------------------------------------------------------

std::string GetUnqualifiedScopelessName(const Type* type);
//-----------------------------------------------------------------------------

std::string
GetTypeNameAsParameter(const QualType& t, std::string_view varName, const Unqualified unqualified = Unqualified::No);
//-----------------------------------------------------------------------------

STRONG_BOOL(WithTemplateParameters);
STRONG_BOOL(IgnoreNamespace);

std::string GetNestedName(const NestedNameSpecifier* nns, const IgnoreNamespace ignoreNamespace = IgnoreNamespace::No);
std::string GetDeclContext(const DeclContext*     ctx,
                           WithTemplateParameters withTemplateParameters = WithTemplateParameters::No);
//-----------------------------------------------------------------------------

const std::string      GetNoExcept(const FunctionDecl& decl);
const std::string_view GetConst(const FunctionDecl& decl);
//-----------------------------------------------------------------------------

std::string GetElaboratedTypeKeyword(const ElaboratedTypeKeyword keyword);
//-----------------------------------------------------------------------------

uint64_t GetSize(const ConstantArrayType*);
//-----------------------------------------------------------------------------

template<typename QT, typename SUB_T>
static bool TypeContainsSubType(const QualType& t)
{
    if(const auto* lref = dyn_cast_or_null<QT>(t.getTypePtrOrNull())) {
        const auto  subType      = GetDesugarType(lref->getPointeeType());
        const auto& ct           = subType.getCanonicalType();
        const auto* plainSubType = ct.getTypePtrOrNull();

        return isa<SUB_T>(plainSubType);
    }

    return false;
}
//-----------------------------------------------------------------------------

template<typename T, typename TFunc>
void for_each(T start, T end, TFunc&& func)
{
    for(; start < end; ++start) {
        func(start);
    }
}
//-----------------------------------------------------------------------------

/// \brief Track the scope we are currently in to build a properly scoped variable.
///
/// The AST only knows about absolute scopes (namespace, struct, class), as once a declaration is parsed it is either in
/// a scope or not. Each request to give me the namespace automatically leads to the entire scope the item is in. This
/// makes it hard to have constructs like this: \code struct One
/// {
///    static const int o{};
///
///    struct Two
///    {
///        static const int d = o;
///    };
///
///    static const int a = Two::d;
///};
/// \endcode
///
/// Here the initializer of \c a is in the scope \c One::Two::d. At this point the qualification \c Two::d is enought.
///
/// \c ScopeHelper tracks whether we are currently in a class or namespace and simply remove the path we already in from
/// the scope.
struct ScopeHelper : public StackListEntry<ScopeHelper>
{
    ScopeHelper(const size_t len)
    : mLength{len}
    {
    }

    const size_t mLength;  //!< Length of the scope as it was _before_ this declaration was appended.
};
//-----------------------------------------------------------------------------

/// \brief The ScopeHandler tracks the current scope.
///
/// The \c ScopeHandler tracks the current scope, knows about all the parts and is able to remove the current scope part
/// from a name.
class ScopeHandler
{
public:
    ScopeHandler(const Decl* d);

    ~ScopeHandler();

    /// \brief Remove the current scope from a string.
    ///
    /// The default is that the entire scope is replaced. Suppose we are
    /// currently in N::X and having a symbol N::X::y then N::X:: is removed. However, there is a special case, where
    /// the last item is skipped.
    static std::string RemoveCurrentScope(std::string name);

private:
    using ScopeStackType = StackList<ScopeHelper>;

    ScopeStackType& mStack;   //!< Access to the global \c ScopeHelper stack.
    ScopeHelper     mHelper;  //!< The \c ScopeHelper this item refers to.

    static inline ScopeStackType mGlobalStack;  //!< Global stack to keep track of the scope elements.
    static inline std::string    mScope;        //!< The entire scope we are already in.
};
//-----------------------------------------------------------------------------

/// \brief Helper to create a \c ScopeHandler on the stack which adds the current \c Decl to it and removes it once the
/// scope is left.
#define SCOPE_HELPER(d)                                                                                                \
    ScopeHandler _scopeHandler                                                                                         \
    {                                                                                                                  \
        d                                                                                                              \
    }
//-----------------------------------------------------------------------------

/// \brief Specialization for \c ::llvm::raw_string_ostream with an internal \c std::string buffer.
class StringStream : public ::llvm::raw_string_ostream
{
private:
    std::string mData{};

public:
    StringStream()
    : ::llvm::raw_string_ostream{mData}
    {
    }

    void Print(const TemplateArgument&);
    void Print(const TemplateSpecializationType&);
    void Print(const TemplateParamObjectDecl&);
    void Print(const TypeConstraint&);
    void Print(const StringLiteral&);
    void Print(const CharacterLiteral&);
};
//-----------------------------------------------------------------------------

/// \brief A helper which invokes a lambda when the scope is destroyed.
template<typename T>
class FinalAction
{
public:
    explicit FinalAction(T&& action)
    : mAction{std::forward<T>(action)}
    {
    }

    ~FinalAction() { mAction(); }

private:
    T mAction;
};
//-----------------------------------------------------------------------------

/// \brief Handy helper to avoid longish comparisons.
///
/// The idea is taken from a talk from Björn Fahller at NDC TechTown 2019: Modern Techniques for Keeping Your Code DRY
/// (https://youtu.be/YUWuNpxZa5k)
/// \code
/// if( is{v}.any_of(A, B, C) ) { ... }
/// \endcode
template<typename T>
struct is
{
    T t;

    constexpr bool any_of(const auto&... ts) const { return ((t == ts) or ...); }
};
//-----------------------------------------------------------------------------

template<typename T>
is(T) -> is<T>;
//-----------------------------------------------------------------------------

/// Go deep in a Stmt if necessary and look to all childs for a DeclRefExpr.
const DeclRefExpr* FindDeclRef(const Stmt* stmt);
//-----------------------------------------------------------------------------

///! Find a LambdaExpr inside a Decltype
class P0315Visitor : public RecursiveASTVisitor<P0315Visitor>
{
    std::variant<std::reference_wrapper<class OutputFormatHelper>, std::reference_wrapper<class CodeGenerator>>
                      mConsumer;
    const LambdaExpr* mLambdaExpr{};

public:
    P0315Visitor(class OutputFormatHelper& ofm)
    : mConsumer{ofm}
    {
    }

    P0315Visitor(class CodeGenerator& cg)
    : mConsumer{cg}
    {
    }

    bool VisitLambdaExpr(const LambdaExpr* expr);

    const LambdaExpr* Get() const { return mLambdaExpr; }
};
//-----------------------------------------------------------------------------

template<typename T, typename U>
struct BackupAndRestore
{
    T& mValue;
    T  mBackup{};

    BackupAndRestore(T& value, U&& newVal)
    : mValue(value)
    , mBackup{mValue}
    {
        mValue = std::forward<U>(newVal);
    }

    ~BackupAndRestore() { mValue = mBackup; }
};
//-----------------------------------------------------------------------------

template<class T>
class MyOptional : public std::optional<T>
{

public:
    using std::optional<T>::optional;

    template<class Return>
    MyOptional<Return> and_then(std::function<MyOptional<Return>(T)> func)
        requires(not std::is_pointer_v<T>)
    {
        if(not this->has_value()) {
            return {};
        }

        return func(this->value());
    }

    template<class Return>
    MyOptional<Return> and_then(std::function<MyOptional<Return>(std::remove_pointer_t<T>&)> func)
        requires(std::is_pointer_v<T>)
    {
        if(not this->has_value()) {
            return {};
        }

        if(this->value() == nullptr) {
            return {};
        } else {
            return func(*this->value());
        }
    }

    template<class Return>
    MyOptional<Return> and_not(std::function<MyOptional<Return>(T)> func)
    {
        if(not this->has_value()) {
            return {};
        }

        if(not func(this->value()).has_value()) {
            return *this;
        }

        return {};
    }
};
//-----------------------------------------------------------------------------

}  // namespace clang::insights

#endif /* INSIGHTS_HELPERS_H */
