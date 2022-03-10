#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Rewrite/Frontend/Rewriters.h"

#include "llvm/Support/raw_ostream.h"

#include <iostream>

using namespace clang;
using namespace llvm;
using namespace std;

using namespace clang::ast_matchers;

// Enum for different types of expression included in our C language subset
// Link: https://tinyurl.com/yc3mzv8o
enum class CSubsetExpr
{
    // Use for initializers
    INVALID,
    // Needed to work around implicit casts
    IMPLICIT_CAST,

    Num,
    Var,
    ParenExpr,
    UnExpr,
    BinExpr,
    Assign,
    CallOrInvocation
};
constexpr const char *CSubsetExprToString(CSubsetExpr CSE)
{
    switch (CSE)
    {
    case CSubsetExpr::INVALID:
        return "INVALID";
    case CSubsetExpr::IMPLICIT_CAST:
        return "IMPLICIT_CAST";
    case CSubsetExpr::Num:
        return "Num";
    case CSubsetExpr::Var:
        return "Var";
    case CSubsetExpr::ParenExpr:
        return "ParenExpr";
    case CSubsetExpr::UnExpr:
        return "UnExpr";
    case CSubsetExpr::BinExpr:
        return "BinExpr";
    case CSubsetExpr::Assign:
        return "Assign";
    case CSubsetExpr::CallOrInvocation:
        return "CallOrInvocation";
    default:
        throw std::invalid_argument("Unimplemented CSubsetExpr type");
    }
}

// TODO: Add transformation of object-like macros to variables to soundness
// proof

// TODO: Refactor and remove global variables

// Source code rewriter
Rewriter RW;

SourceManager *SM;

Preprocessor *PP;

LangOptions *LO;

// Set of all variable nams declared in a program
set<string> AllVarNames;
// Set of all function names declared in a program
set<string> AllFunctionNames;
// Set of all macro names declared in a program
set<string> AllMacroNames;
// Set of all multiply-defined macros
set<string> MultiplyDefinedMacros;
// List of all macro expansion ranges
list<SourceRange> ExpansionRanges;
// Set of starting locations for all expansion ranges that contain
// the start of another locations
set<SourceLocation> StartLocationsOfExpansionsContainingStartOfOtherExpansion;
// Set of starting locations for all expansion ranges that are contained
// within another expansion
set<SourceLocation> StartLocationsOFExpansionsContainedInOtherExpansion;
// Mapping from starting locations of macro expansions to names of all macros
// starting at that location
map<SourceLocation, list<string>> ExpansionStartLocationToMacroNames;
// Mapping from macro names to list of corresponding expansion ranges
map<string, list<SourceRange>> MacroNameToExpansionRanges;

// Mapping from macro names to their directives
map<string, const MacroDirective *> MacroNameToDirective;

// Map for memoizing results of isExprInCSubset
map<Expr *, bool> EInCSub;

// Map for memoizing results of EToCSub
map<Expr *, CSubsetExpr> EToCSub;

// Map for memoizing results of exprContainsLocalVars
map<Expr *, bool> EContainsLocalVars;

// Map for memoizing results of exprHasSideEffects
map<Expr *, bool> EHasSideEffects;

// Kinds of smart pointers;
// https://tinyurl.com/y8hbbdwq

// Returns true if a given source location is contained within a
// a given source range
bool isInRange(SourceLocation &L, SourceRange &SR2)
{
    SourceLocation B2 = SR2.getBegin();
    SourceLocation E2 = SR2.getEnd();

    return B2 <= L && L <= E2;
}

// Preprocessor callback to collect information about macro expansions
class MacroExpansionCollector : public PPCallbacks
{
public:
    void MacroExpands(const Token &MacroNameTok,
                      const MacroDefinition &MD,
                      SourceRange Range,
                      const MacroArgs *Args)
    {
        string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
        // errs() << "Found macro expansion: " << MacroName << "\n";

        auto ExpansionRange = SM->getExpansionRange(Range).getAsRange();
        auto B = ExpansionRange.getBegin();
        for (auto &&OtherExpansionRange : ExpansionRanges)
        {
            auto OB = OtherExpansionRange.getBegin();
            if (isInRange(B, OtherExpansionRange))
            {
                list<string> OtherMacroNames =
                    ExpansionStartLocationToMacroNames[OtherExpansionRange.getBegin()];
                if (OtherMacroNames.size() == 0)
                {
                    // errs() << "Error: " << MacroName
                    //        << " was found to expand"
                    //           " within another expansion, but no"
                    //           " other expansion could be found\n ";
                }
                else if (OtherMacroNames.size() == 1)
                {
                    // errs() << MacroName << " expands in "
                    //        << OtherMacroNames.front() << "\n";
                }
                else if (OtherMacroNames.size() > 1)
                {
                    // errs() << MacroName
                    //        << " expands in another expansion,"
                    //           " but it could not be determined specifically"
                    //           " which one\n";
                }
                StartLocationsOfExpansionsContainingStartOfOtherExpansion.insert(OB);
                StartLocationsOFExpansionsContainedInOtherExpansion.insert(B);
            }
        }

        ExpansionStartLocationToMacroNames[B].push_back(MacroName);
        MacroNameToExpansionRanges[MacroName].push_back(ExpansionRange);
        ExpansionRanges.push_back(ExpansionRange);
    }
};

// Preprocessor callback for collecting macro definitions
class MacroDefinitionCollector : public PPCallbacks
{
public:
    // Hook called whenever a macro definition is seen.
    void MacroDefined(const Token &MacroNameTok, const MacroDirective *MD)
    {
        IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
        string MacroName = II->getName().str();

        // Add this macro name to the set of macro names used in the program
        AllMacroNames.insert(MacroName);

        // Check if this macros is multiply-defined
        if (MD->getPrevious() != nullptr)
        {
            MultiplyDefinedMacros.insert(MacroName);
        }

        // Map macro name to its directive
        // It's fine if we overwrite a macro, because we only transform
        // macros that are not multiply-defined
        MacroNameToDirective[MacroName] = MD;
    }
};

// Visitor class which collects the names of all variables and functions
// declared in a program
class CollectDeclNamesVisitor
    : public RecursiveASTVisitor<CollectDeclNamesVisitor>
{
private:
    ASTContext *Ctx;

public:
    explicit CollectDeclNamesVisitor(CompilerInstance *CI)
        : Ctx(&(CI->getASTContext()))
    {
    }

    bool VisitFunctionDecl(FunctionDecl *FDecl)
    {
        string functionName = FDecl
                                  ->getNameInfo()
                                  .getName()
                                  .getAsString();
        AllFunctionNames.insert(functionName);

        return true;
    }

    bool VisitVarDecl(VarDecl *VD)
    {
        string VarName = VD->getName().str();
        AllVarNames.insert(VarName);
        return true;
    }
};

// Returns true if the given variable declaration is for a global variable,
// false otherwise
bool isGlobalVariable(VarDecl *VD)
{
    return VD->hasGlobalStorage() && !VD->isStaticLocal();
}

// Returns true if the given expression is in our C language subset,
// false otherwise.
bool isExprInCSubset(Expr *E)
{
    // We use a map to memoize results
    auto Entry = EInCSub.find(E);
    if (Entry != EInCSub.end())
    {
        // errs() << "Expression already checked: ";
        // E->dumpColor();
        return Entry->second;
    }

    bool result = false;

    // IMPLICIT
    if (auto Imp = dyn_cast<ImplicitCastExpr>(E))
    {
        if (auto E0 = Imp->getSubExpr())
        {
            result = isExprInCSubset(E0);
        }
    }
    // Num
    else if (auto Num = dyn_cast<IntegerLiteral>(E))
    {
        result = true;
    }
    // Var
    else if (auto DRF = dyn_cast<clang::DeclRefExpr>(E))
    {
        if (auto Var = dyn_cast_or_null<VarDecl>(DRF->getDecl()))
        {
            if (Var->getType().getAsString().compare("int") == 0)
            {
                result = true;
            }
        }
    }
    // ParenExpr
    else if (auto ParenExpr_ = dyn_cast<ParenExpr>(E))
    {
        if (auto E0 = ParenExpr_->getSubExpr())
        {
            result = isExprInCSubset(E0);
        }
    }
    // UnExpr
    else if (auto UnExpr = dyn_cast<clang::UnaryOperator>(E))
    {
        auto OC = UnExpr->getOpcode();
        if (OC == UO_Plus || OC == UO_Minus)
        {
            if (auto E0 = UnExpr->getSubExpr())
            {
                result = isExprInCSubset(E0);
            }
        }
    }
    // BinExpr
    else if (auto BinExpr = dyn_cast<clang::BinaryOperator>(E))
    {
        auto OC = BinExpr->getOpcode();
        if (OC == BO_Add || OC == BO_Sub || OC == BO_Mul || OC == BO_Div)
        {
            auto E1 = BinExpr->getLHS();
            auto E2 = BinExpr->getRHS();
            if (E1 && E2)
            {
                result = isExprInCSubset(E1) && isExprInCSubset(E2);
            }
        }
        // Assign
        else if (OC == BO_Assign)
        {
            // Can we just use dyn_cast here (Can the LHS be NULL?)?
            if (auto X = dyn_cast_or_null<DeclRefExpr>(BinExpr->getLHS()))
            {
                // Check that LHS is just a variable
                if (isa<VarDecl>(X->getDecl()))
                {
                    auto E2 = BinExpr->getRHS();
                    result = isExprInCSubset(E2);
                }
            }
        }
    }
    // CallOrInvocation (function call)
    else if (auto CallOrInvocation = dyn_cast<CallExpr>(E))
    {
        // NOTE: This extends the Coq language by including function calls
        // which have arguments that are not in the language
        result = true;
    }
    EInCSub[E] = result;
    return result;
}

// Returns true if the given expression contains any non-global variables,
// false otherwise
bool exprContainsLocalVars(Expr *E)
{
    // We use a map to memoize results
    if (EContainsLocalVars.find(E) != EContainsLocalVars.end())
    {
        return EContainsLocalVars[E];
    }

    bool result = true;

    // IMPLICIT
    if (auto Imp = dyn_cast<ImplicitCastExpr>(E))
    {
        if (auto E0 = Imp->getSubExpr())
        {
            result = exprContainsLocalVars(E0);
        }
    }
    // Num
    else if (auto Num = dyn_cast<IntegerLiteral>(E))
    {
        result = false;
    }
    // Var
    else if (auto Var = dyn_cast<clang::DeclRefExpr>(E))
    {
        if (auto VD = dyn_cast<VarDecl>(Var->getDecl()))
        {
            result = !isGlobalVariable(VD);
        }
    }
    // ParenExpr
    else if (auto ParenExpr_ = dyn_cast<ParenExpr>(E))
    {
        if (auto E0 = ParenExpr_->getSubExpr())
        {
            result = exprContainsLocalVars(E0);
        }
    }
    // UnExpr
    else if (auto UnExpr = dyn_cast<clang::UnaryOperator>(E))
    {
        auto OC = UnExpr->getOpcode();
        if (OC == UO_Plus || OC == UO_Minus)
        {
            if (auto E0 = UnExpr->getSubExpr())
            {
                result = exprContainsLocalVars(E0);
            }
        }
    }
    // BinExpr
    else if (auto BinExpr = dyn_cast<clang::BinaryOperator>(E))
    {
        auto OC = BinExpr->getOpcode();
        if (OC == BO_Add || OC == BO_Sub || OC == BO_Mul || OC == BO_Div)
        {
            auto E1 = BinExpr->getLHS();
            auto E2 = BinExpr->getRHS();
            if (E1 && E2)
            {
                result = exprContainsLocalVars(E1) ||
                         exprContainsLocalVars(E2);
            }
        }
        // Assign
        else if (OC == BO_Assign)
        {
            // Can we just use dyn_cast here (Can the LHS be NULL?)
            if (auto X = dyn_cast_or_null<DeclRefExpr>(BinExpr->getLHS()))
            {
                // TODO: Ensure that the LHS is a var
                if (auto VD = dyn_cast<VarDecl>(X->getDecl()))
                {
                    result = !isGlobalVariable(VD);
                    // If the variable being assigned to is not a local var,
                    // then we must still check the RHS for a local var
                    auto E2 = BinExpr->getRHS();
                    result = result || exprContainsLocalVars(E2);
                }
            }
        }
    }
    // CallOrInvocation (function call)
    else if (auto CallOrInvocation = dyn_cast<CallExpr>(E))
    {
        result = false;
        for (auto &&Arg : CallOrInvocation->arguments())
        {
            if (exprContainsLocalVars(Arg))
            {
                result = true;
                break;
            }
        }
    }
    EContainsLocalVars[E] = result;
    return result;
}

// Returns true if the given expression may have side-effects, false otherwise.
// Clang offers this function, but we use our own implementation for two
// reasons: 1) To ensure that we match the formal work. 2) To avoid passing the
// AST context to all transformation functions
bool exprHasSideEffects(Expr *E)
{
    // We use a map to memoize results
    if (EHasSideEffects.find(E) != EHasSideEffects.end())
    {
        return EHasSideEffects[E];
    }

    bool result = true;

    // IMPLICIT
    if (auto Imp = dyn_cast<ImplicitCastExpr>(E))
    {
        if (auto E0 = Imp->getSubExpr())
        {
            result = exprHasSideEffects(E0);
        }
    }
    // Num
    else if (auto Num = dyn_cast<IntegerLiteral>(E))
    {
        result = false;
    }
    // Var
    else if (auto Var = dyn_cast<clang::DeclRefExpr>(E))
    {
        result = false;
    }
    // ParenExpr
    else if (auto ParenExpr_ = dyn_cast<ParenExpr>(E))
    {
        if (auto E0 = ParenExpr_->getSubExpr())
        {
            result = exprHasSideEffects(E0);
        }
    }
    // UnExpr
    else if (auto UnExpr = dyn_cast<clang::UnaryOperator>(E))
    {
        auto OC = UnExpr->getOpcode();
        if (OC == UO_Plus || OC == UO_Minus)
        {
            if (auto E0 = UnExpr->getSubExpr())
            {
                result = exprHasSideEffects(E0);
            }
        }
    }
    // BinExpr
    else if (auto BinExpr = dyn_cast<clang::BinaryOperator>(E))
    {
        auto OC = BinExpr->getOpcode();
        if (OC == BO_Add || OC == BO_Sub || OC == BO_Mul || OC == BO_Div)
        {
            auto E1 = BinExpr->getLHS();
            auto E2 = BinExpr->getRHS();
            if (E1 && E2)
            {
                result = exprHasSideEffects(E1) || exprHasSideEffects(E2);
            }
        }
        // Assign
        else if (OC == BO_Assign)
        {
            result = true;
        }
    }
    // CallOrInvocation (function call)
    else if (auto CallOrInvocation = dyn_cast<CallExpr>(E))
    {
        result = true;
    }
    EHasSideEffects[E] = result;
    return result;
}

// Returns the C language subset syntax node that this expression
// corresponds to
CSubsetExpr classifyExpr(Expr *E)
{
    // We use a map to memoize results
    if (EToCSub.find(E) != EToCSub.end())
    {
        return EToCSub[E];
    }

    CSubsetExpr result = CSubsetExpr::INVALID;

    if (isExprInCSubset(E))
    {
        // At this point, since we know the expression is in the language
        // subset, we only have to perform minimal checks to determine
        // what type of subset expression this expression is

        // IMPLICIT
        // NOTE: Should we record the fact that this expression was found
        // under an implicit cast?
        if (auto Imp = dyn_cast<ImplicitCastExpr>(E))
        {
            if (auto E0 = Imp->getSubExpr())
            {
                result = classifyExpr(E0);
            }
        }
        // Num
        else if (auto Num = dyn_cast<IntegerLiteral>(E))
        {
            result = CSubsetExpr::Num;
        }
        // Var
        else if (auto Var = dyn_cast<clang::DeclRefExpr>(E))
        {
            result = CSubsetExpr::Var;
        }
        // ParenExpr
        else if (auto ParenExpr_ = dyn_cast<ParenExpr>(E))
        {
            if (auto E0 = ParenExpr_->getSubExpr())
            {
                result = CSubsetExpr::ParenExpr;
            }
        }
        // UnExpr
        else if (auto UnExpr = dyn_cast<clang::UnaryOperator>(E))
        {
            auto OC = UnExpr->getOpcode();
            if (OC == UO_Plus || OC == UO_Minus)
            {
                if (auto E0 = UnExpr->getSubExpr())
                {
                    result = CSubsetExpr::UnExpr;
                }
            }
        }
        // BinExpr
        else if (auto BinExpr = dyn_cast<clang::BinaryOperator>(E))
        {
            auto OC = BinExpr->getOpcode();
            if (OC != BO_Assign)
            {
                result = CSubsetExpr::BinExpr;
            }
            // Assign
            else
            {
                result = CSubsetExpr::Assign;
            }
        }
        // CallOrInvocation (function call)
        else if (auto CallOrInvocation = dyn_cast<CallExpr>(E))
        {
            // NOTE: This extends the Coq language by including function calls
            // which have arguments that are not in the language
            result = CSubsetExpr::CallOrInvocation;
        }
    }

    EToCSub[E] = result;
    return result;
}

// Determines if an expression is a result of macro expansion,
// and if so, then tries to transform the invocation into a function call.
// Returns true if the invocation was transformed; false if the expression
// was not the result of a macro expansion or if the invocation was not
// transformed for some other reason
// TODO
enum class TransformationResult
{
    CONTAINS_NESTED_INVOCATIONS,
    CONTAINED_IN_INVOCATION,
    ERROR,
    HAS_SIDE_EFFECTS,
    MULTIPLE_EXPANSIONS,
    MULTIPLY_DEFINED,
    NON_TRANSFORMABLE_MACRO,
    NOT_TRANSFORMED,
    TRANSFORMED
};
TransformationResult transformEntireExpr(Expr *E)
{
    // Check if macro is hygienic
    // Check if the entire expression came from a macro expansion
    auto B = E->getBeginLoc();
    auto EL = E->getEndLoc();
    string MacroName = "";
    // Note: This checks that the beginning of the expression
    // and end of the expression came from macro invocations, but doesn'y
    // guarantee that they came from the *same* invocation.
    // We do that farther down
    if (PP->isAtStartOfMacroExpansion(B) &&
        PP->isAtEndOfMacroExpansion(EL))
    {
        // Get the beginning of the expansion
        // errs() << "Found an expression that begins at an expansion\n";
        auto ER = SM->getExpansionRange(E->getSourceRange()).getAsRange();
        auto EB = ER.getBegin();

        // Don't transform transform expansions with nested expansions
        if (StartLocationsOfExpansionsContainingStartOfOtherExpansion.find(EB) !=
            StartLocationsOfExpansionsContainingStartOfOtherExpansion.end())
        {
            errs() << "Found an invocation with nested invocations\n";
            return TransformationResult::CONTAINS_NESTED_INVOCATIONS;
        }

        // Don't transform nested expansions
        // FIXME: Not necessary but would be nice if this worked
        // NOTE: Actually since we don't recursively visit nested invocations,
        // we should never encounter this case.
        if (StartLocationsOFExpansionsContainedInOtherExpansion.find(EB) !=
            StartLocationsOFExpansionsContainedInOtherExpansion.end())
        {
            errs() << "Found a nested invocation\n";
            return TransformationResult::CONTAINED_IN_INVOCATION;
        }

        // Try to unambiguously determine the macro
        // that this expansion refers to
        if (ExpansionStartLocationToMacroNames.find(EB) !=
            ExpansionStartLocationToMacroNames.end())
        {
            list<string> NamesOfMacrosExpandingStartingHere =
                ExpansionStartLocationToMacroNames[EB];
            if (NamesOfMacrosExpandingStartingHere.size() == 0)
            {
                errs() << "Error: Clang reported a macro invocation"
                          " at this location but none found\n";
                return TransformationResult::ERROR;
            }
            else if (NamesOfMacrosExpandingStartingHere.size() == 1)
            {
                // Verify that entire expression came from a single expansion
                // This we check that the beginning and end of the expression
                // came from the *same* invocation
                MacroName = NamesOfMacrosExpandingStartingHere.front();
                bool cameFromSingleExpansion = false;
                list<SourceRange> MacroExpansionRanges =
                    MacroNameToExpansionRanges[MacroName];
                for (auto &&MER : MacroExpansionRanges)
                {
                    if (ER == MER)
                    {
                        E->dumpColor();
                        cameFromSingleExpansion = true;
                        break;
                    }
                }

                if (!cameFromSingleExpansion)
                {
                    errs() << "Found an expression composed of multiple "
                           << "distinct expansions\n";
                    return TransformationResult::MULTIPLE_EXPANSIONS;
                }

                errs() << "Found an unambiguous invocation of "
                       << MacroName << "\n";
            }
            else if (NamesOfMacrosExpandingStartingHere.size() > 1)
            {
                errs() << "Could not unambiguously determine"
                       << " macro invocation to transform\n";
                return TransformationResult::ERROR;
            }
        }
    }
    else
    {
        errs() << "Found an expression which began at a macro expansion, "
               << "but did not end at one\n";
        return TransformationResult::NOT_TRANSFORMED;
    }

    // Sanity check
    if (MacroName == "")
    {
        errs() << "Found a macro that should have had a name, but did not\n";
        return TransformationResult::ERROR;
    }

    // Check that invoked macro is not multiply-defined
    if (MultiplyDefinedMacros.find(MacroName) != MultiplyDefinedMacros.end())
    {
        errs() << "Found a multiply-defined macro\n";
        return TransformationResult::MULTIPLY_DEFINED;
    }
    const MacroDirective *MD = MacroNameToDirective[MacroName];

    // Check that the invoked macro is an object-like macro or a nullary
    // function-like macro
    const MacroInfo *MI = MD->getMacroInfo();
    if (MI->isFunctionLike() && MI->getNumParams() > 1)
    {
        errs() << "Found a function-like macro invocation with more than "
                  "one argument \n";
        return TransformationResult::NON_TRANSFORMABLE_MACRO;
    }

    // If the macro has a single parameter, check that that parameter
    // comprises the entire definition of the macro
    if (MI->isFunctionLike() && MI->getNumParams() == 1)
    {
        errs() << "Found a function-like macro invocation with 1 argument\n";
        // Verify that the macro's definition is just its argument

        auto DefLen = MI->getDefinitionLength(*SM);
        auto ParamLen = MI->params().front()->getLength();

        if (DefLen != ParamLen)
        {
            errs() << "Found a function-like macro with 1 argument that was "
                      "not the ID macro\n";
            return TransformationResult::NON_TRANSFORMABLE_MACRO;
        }
    }

    // Check that the expression does not have side-effects
    if (exprHasSideEffects(E))
    {
        errs() << "Found a macro invocation with side-effects\n";
        return TransformationResult::HAS_SIDE_EFFECTS;
    }

    // Check that the expression does not share variables with the
    // caller environment
    if (exprContainsLocalVars(E))
    {
        errs() << "Found an expression containing local or captured var(s)\n";
        return TransformationResult::NOT_TRANSFORMED;
    }

    // Give the transformed macro a unique name
    string suffix = MI->isObjectLike() ? "_var" : "_function";
    string DefName = MacroName + suffix;
    unsigned int i = 0;
    while (AllVarNames.find(DefName) != AllVarNames.end() ||
           AllFunctionNames.find(DefName) != AllFunctionNames.end() ||
           AllMacroNames.find(DefName) != AllMacroNames.end())
    {
        DefName = MacroName + suffix + to_string(i);
        i += 1;
    }

    // Get location for where to insert transformed macro
    SourceLocation MacroDefEnd = MI->getDefinitionEndLoc();
    SourceLocation DefLocation =
        Lexer::getLocForEndOfToken(MacroDefEnd, 0, *SM, *LO);

    // Get the body of the definition
    SourceLocation MacroDefBegin = MI->getDefinitionLoc();
    // Skip the name of the defined macro
    SourceLocation MacroBodyBegin =
        Lexer::getLocForEndOfToken(MacroDefBegin, 0, *SM, *LO);
    // Go to end of formals for nullary function-like macro
    if (MI->isFunctionLike())
    {
        if (MI->getNumParams() == 0)
        {
            MacroBodyBegin = MacroBodyBegin.getLocWithOffset(2);
        }
        // Go to end of formals for macro with 1 parameter
        else if (MI->getNumParams() == 1)
        {
            unsigned int offset = 0;
            // Add 1 for opening parenthesis
            offset += 1;

            // Add parameter name length
            auto Param = MI->params().front();
            offset += Param->getLength();

            // Add 1 for closing parenthesis
            offset += 1;
            MacroBodyBegin = MacroBodyBegin.getLocWithOffset(offset);
        }
        else
        {
            errs() << "Somehow tried to transform a macro with more than 1 "
                      "parameter\n";
            return TransformationResult::ERROR;
        }
    }

    // Skip leading space in macro definition
    MacroBodyBegin = MacroBodyBegin.getLocWithOffset(1);

    SourceRange MacroBodyRange = SourceRange(MacroBodyBegin, MacroDefEnd);
    CharSourceRange MacroDefRange = Lexer::getAsCharRange(
        MacroBodyRange, *SM, *LO);
    bool *RWError = nullptr;
    string DefBody = Lexer::getSourceText(MacroDefRange, *SM, *LO, RWError)
                         .str();
    if (RWError != nullptr)
    {
        errs() << "Could not get source text of macro definition\n";
        return TransformationResult::ERROR;
    }

    // TODO: Check if a macro was previously transformed with the same
    // name and body, and if so, don't emit a new definition but use that
    // one instead

    string Def = "";
    if (MI->isObjectLike())
    {
        Def = "\nconst int " + DefName + " = " + DefBody + ";";
        AllVarNames.insert(DefName);
    }
    else
    {
        if (MI->getNumParams() == 0)
        {
            Def = "\nint " + DefName + "() {\n    return " + DefBody + ";\n}";
        }
        else if (MI->getNumParams() == 1)
        {
            auto Param = MI->params().front();
            string ParamName = Param->getName().str();
            Def = "\nint " + DefName + "(int " + ParamName + ") " +
                  "{\n    return " + DefBody + ";\n}";
        }
        else
        {
            errs() << "Tried to transform a macro with more than 1 argument\n";
            return TransformationResult::ERROR;
        }

        // Add the function name to the name of all functions defined in
        // the program
        AllFunctionNames.insert(DefName);
    }
    // Sanity check
    if (Def.compare("") == 0)
    {
        errs() << "Macro body was not defined\n";
        return TransformationResult::ERROR;
    }

    if (RW.InsertTextAfter(DefLocation, StringRef(Def)))
    {
        errs() << "Rewriter could not rewrite macro\n";
        return TransformationResult::ERROR;
    }

    // Create the replacement for the invocation
    string InvocationReplacement = DefName;

    if (MI->isFunctionLike())
    {
        if (MI->getNumParams() == 0)
        {
            InvocationReplacement += "()";
        }
        else if (MI->getNumParams() == 1)
        {
            // Add opening paren
            InvocationReplacement += "(";

            // Add argument
            // NOTE: This will only work if the macro is the ID macro
            string InvocationString = Lexer::getSourceText(
                                          SM->getExpansionRange(B),
                                          *SM, *LO)
                                          .str();
            unsigned int ArgLen = InvocationString.length() -
                                  1 - MacroName.length() - 1;
            string ArgString = InvocationString.substr(
                MacroName.length() + 1,
                ArgLen);
            InvocationReplacement += ArgString;

            // Add closing paren
            InvocationReplacement += ")";
        }
        else
        {
            errs() << "Tried to transform a macro with more than 1 argument\n";
            return TransformationResult::ERROR;
        }
    }

    // Get expansion range of expression
    SourceRange ER = SM->getExpansionRange(E->getSourceRange()).getAsRange();

    // Transform the macro invocation into a variable reference or
    // function call
    if (RW.ReplaceText(ER, StringRef(InvocationReplacement)))
    {
        errs() << "Could not transform invocation of macro\n";
        return TransformationResult::ERROR;
    }
    errs() << "Successfully transformed a macro\n";
    return TransformationResult::TRANSFORMED;
}

// NOTE:
// These functions are it - The trick now is to extract potential
// macro invocations from expressions
void TransformExpr(Expr *E);
void TransformStmt(Stmt *S);
void TransformProgram(TranslationUnitDecl *TUD);

// Transforms all eligible macro invocations in the given expression into
// C function calls
void TransformExpr(Expr *E)
{
    // Step 1: Classify the expression
    CSubsetExpr CSE = classifyExpr(E);

    // Don't transform expressions not in the language
    if (CSE == CSubsetExpr::INVALID)
    {
        // errs() << "Found an expression not in the language\n";
        return;
    }

    // Step 2: Try to transform the entire expression
    errs() << "Transforming a " << CSubsetExprToString(CSE) << "\n";
    TransformationResult result = transformEntireExpr(E);

    // Step 3: If we could not transform the entire expression,
    // then try to transform its subexpressions.
    // Note that we don't have to check subexpressions for being in
    // the language subset since isExprInCSubset handles that recursively
    if (result == TransformationResult::NOT_TRANSFORMED ||
        result == TransformationResult::MULTIPLY_DEFINED ||
        result == TransformationResult::HAS_SIDE_EFFECTS ||
        result == TransformationResult::MULTIPLE_EXPANSIONS)
    {
        // errs() << "No change made to " << CSubsetExprToString(CSE) << "\n";
        // IMPLICIT
        if (auto Imp = dyn_cast<ImplicitCastExpr>(E))
        {
            if (auto E0 = Imp->getSubExpr())
            {
                TransformStmt(E0);
            }
        }
        // Num
        else if (auto Num = dyn_cast<IntegerLiteral>(E))
        {
            // Do nothing since Num does not have any subexpressions
        }
        // Var
        else if (auto Var = dyn_cast<clang::DeclRefExpr>(E))
        {
            // Do nothing since Var does not have any subexpressions
        }
        // ParenExpr
        else if (auto ParenExpr_ = dyn_cast<ParenExpr>(E))
        {
            if (auto E0 = ParenExpr_->getSubExpr())
            {
                TransformStmt(E0);
            }
        }
        // UnExpr
        else if (auto UnExpr = dyn_cast<clang::UnaryOperator>(E))
        {
            auto OC = UnExpr->getOpcode();
            if (OC == UO_Plus || OC == UO_Minus)
            {
                if (auto E0 = UnExpr->getSubExpr())
                {
                    TransformExpr(E0);
                }
            }
        }
        // BinExpr
        else if (auto BinExpr = dyn_cast<clang::BinaryOperator>(E))
        {
            auto OC = BinExpr->getOpcode();
            if (OC == BO_Add || OC == BO_Sub || OC == BO_Mul || OC == BO_Div)
            {
                auto E1 = BinExpr->getLHS();
                auto E2 = BinExpr->getRHS();
                if (E1 && E2)
                {
                    TransformExpr(E1);
                    TransformExpr(E2);
                }
            }
            // Assign
            else if (OC == BO_Assign)
            {
                // Can we just use dyn_cast here (Can the LHS be NULL?)
                if (auto X = dyn_cast_or_null<DeclRefExpr>(BinExpr->getLHS()))
                {
                    auto E2 = BinExpr->getRHS();
                    TransformExpr(E2);
                }
            }
        }
        // CallOrInvocation (function call)
        else if (auto CallOrInvocation = dyn_cast<CallExpr>(E))
        {
            for (auto &&Arg : CallOrInvocation->arguments())
            {
                TransformExpr(Arg);
            }
        }
    }
}

// Transforms all eligible macro invocations in the given statement into
// C function calls
void TransformStmt(Stmt *S)
{
    // Note: Should we not transfor a stmt at all if any of its
    // substatements are not in the C language subset?

    // ExprStmt
    if (auto ES = dyn_cast<Expr>(S))
    {
        // errs() << "Transforming an ExprStmt\n";
        TransformExpr(ES);
    }
    // IfElseStmt
    else if (auto IfElseStmt = dyn_cast<IfStmt>(S))
    {
        // Check for else branch
        Expr *E = IfElseStmt->getCond();
        Stmt *S1 = IfElseStmt->getThen();
        Stmt *S2 = IfElseStmt->getElse();
        if (E && S1 && S2)
        {
            // errs() << "Transforming an IfElseStmt\n";
            TransformExpr(E);
            TransformStmt(S1);
            TransformStmt(S2);
        }
    }
    // WhileStmt
    else if (auto WhileStmt_ = dyn_cast<WhileStmt>(S))
    {
        Expr *E = WhileStmt_->getCond();
        Stmt *S1 = WhileStmt_->getBody();
        if (E && S1)
        {
            // errs() << "Transforming a WhileStmt\n";
            TransformExpr(E);
            TransformStmt(S1);
        }
    }
    // CompoundStmt
    else if (auto CS = dyn_cast<CompoundStmt>(S))
    {
        // errs() << "Transforming a CompoundStmt\n";
        for (auto &&S : CS->children())
        {
            TransformStmt(S);
        }
    }
}

// Transforms all eligible macro invocations in a program into C function calls
void TransformProgram(TranslationUnitDecl *TUD, SourceManager &SM)
{
    // This probably won't happen, but to be safe
    if (!TUD)
    {
        return;
    }

    // Visit all function definitions in the program
    for (auto D : TUD->decls())
    {
        SourceLocation L = D->getLocation();
        // Check that this definition is in the main file
        // Not sure if we should use this condition or the one below it
        // if (!SM.isWrittenInMainFile)
        if (!SM.isInMainFile(L))
        {
            continue;
        }

        if (auto FD = dyn_cast<FunctionDecl>(D))
        {
            if (FD->isThisDeclarationADefinition())
            {
                // errs() << "Transforming a function definition\n";
                auto FBody = FD->getBody();
                TransformStmt(FBody);
            }
        }
    }
}

// AST consumer which calls the visitor class to perform the transformation
class CppToCConsumer : public ASTConsumer
{
private:
    CompilerInstance *CI;

public:
    explicit CppToCConsumer(CompilerInstance *CI)
        : CI(CI) {}

    virtual void HandleTranslationUnit(ASTContext &Ctx)
    {
        auto begin_time = std::chrono::high_resolution_clock::now();

        TranslationUnitDecl *TUD = Ctx.getTranslationUnitDecl();
        // Collect the names of all the variables and functions
        // defined in the program
        CollectDeclNamesVisitor CDNvisitor(CI);
        CDNvisitor.TraverseTranslationUnitDecl(TUD);

        // Transform the program
        TransformProgram(TUD, Ctx.getSourceManager());

        // Print the results of the rewriting for the current file
        const RewriteBuffer *RewriteBuf =
            RW.getRewriteBufferFor(Ctx.getSourceManager().getMainFileID());
        if (RewriteBuf != nullptr)
        {
            RewriteBuf->write(outs());
        }
        else
        {
            outs() << "No changes to AST\n";
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                            end_time - begin_time)
                            .count();
        errs() << "Finished in " << duration << " microseconds."
               << "\n";
    }
};

// Wrap everything into a plugin
class PluginCppToCAction : public PluginASTAction
{

protected:
    unique_ptr<ASTConsumer>
    CreateASTConsumer(CompilerInstance &CI,
                      StringRef file) override
    {
        // Initialize global variables
        // TODO: Make these vars local instead
        SM = &(CI.getSourceManager());
        PP = &(CI.getPreprocessor());
        LO = &(CI.getLangOpts());
        // Important!
        RW.setSourceMgr(*SM, *LO);

        // Collect macro definition and expansion info
        MacroExpansionCollector *MIC = new MacroExpansionCollector();
        MacroDefinitionCollector *MDC = new MacroDefinitionCollector();
        PP->addPPCallbacks(unique_ptr<PPCallbacks>(MIC));
        PP->addPPCallbacks(unique_ptr<PPCallbacks>(MDC));

        // Return the consumer to initiate the transformation
        return make_unique<CppToCConsumer>(&CI);
    }

    bool ParseArgs(const CompilerInstance &CI,
                   const vector<string> &args) override
    {
        return true;
    }

    // Necessary for ANYTHING to print to stderr
    ActionType getActionType() override
    {
        return ActionType::AddBeforeMainAction;
    }
};

//-----------------------------------------------------------------------------
// Registration
//-----------------------------------------------------------------------------
static FrontendPluginRegistry::Add<PluginCppToCAction>
    X("cpp-to-c", "Transform CPP macros to C functions");
