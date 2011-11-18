//===--- TypeCheckDecl.cpp - Type Checking for Declarations ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for declarations.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "llvm/ADT/Twine.h"
using namespace swift;

namespace {
class DeclChecker : public DeclVisitor<DeclChecker> {
public:
  TypeChecker &TC;
  
  DeclChecker(TypeChecker &TC) : TC(TC) {}

  //===--------------------------------------------------------------------===//
  // Helper Functions.
  //===--------------------------------------------------------------------===//

  bool visitValueDecl(ValueDecl *VD);
  bool validateVarName(Type Ty, DeclVarName *Name);
  void validateAttributes(ValueDecl *VD);

  //===--------------------------------------------------------------------===//
  // Visit Methods.
  //===--------------------------------------------------------------------===//

  void visitImportDecl(ImportDecl *ID) {
    // Nothing to do.
  }
  
  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    TC.validateType(TAD->getAliasType());
  }

  void visitVarDecl(VarDecl *VD) {
    // Type check the ValueDecl part of a VarDecl.
    if (visitValueDecl(VD))
      return;
    
    // If the VarDecl had a name specifier, verify that it lines up with the
    // actual type of the VarDecl.
    if (VD->getNestedName() && validateVarName(VD->getType(), VD->getNestedName()))
      VD->setNestedName(nullptr);
  }
  
  void visitFuncDecl(FuncDecl *FD) {
    visitValueDecl(FD);
  }
  void visitOneOfElementDecl(OneOfElementDecl *OOED) {
    // No type checking required?
  }
  void visitArgDecl(ArgDecl *AD) {
    llvm_unreachable("ArgDecls should never exist in a statement");
  }
  
  void visitElementRefDecl(ElementRefDecl *ERD) {
    // If the type is already resolved we're done.  ElementRefDecls are
    // simple.
    if (!ERD->getType()->is<DependentType>()) return;
    
    if (Type T = ElementRefDecl::getTypeForPath(ERD->VD->getType(),
                                                ERD->AccessPath)) {
      ERD->overwriteType(T);
    } else {
      TC.diagnose(ERD->getLocStart(), diag::invalid_index_in_element_ref,
                  ERD->getName(), ERD->VD->getType());
      ERD->overwriteType(ErrorType::get(TC.Context));
    }
  }
};
}; // end anonymous namespace.


void TypeChecker::typeCheckDecl(Decl *D) {
  DeclChecker(*this).visit(D);
}

bool DeclChecker::visitValueDecl(ValueDecl *VD) {
  if (TC.validateType(VD)) {
    VD->setInit(nullptr);
    return true;
  }
  
  // Validate that the initializers type matches the expected type.
  if (!VD->getInit()) {
    // If we have no initializer and the type is dependent, then the initializer
    // was invalid and removed.
    if (VD->getType()->is<DependentType>())
      return true;
  } else {
    Type DestTy = VD->getType();
    if (DestTy->is<DependentType>())
      DestTy = Type();
    if (!TC.typeCheckExpression(VD->getInitRef(), DestTy))
      VD->overwriteType(VD->getInit()->getType());
    else if (isa<VarDecl>(VD))
      TC.diagnose(VD->getLocStart(), diag::while_converting_var_init);
  }
  
  validateAttributes(VD);
  return false;
}


/// validateAttributes - Check that the func/var declaration attributes are ok.
void DeclChecker::validateAttributes(ValueDecl *VD) {
  DeclAttributes &Attrs = VD->getAttrs();
  Type Ty = VD->getType();
  
  // Get the number of lexical arguments, for semantic checks below.
  int NumArguments = -1;
  if (FunctionType *FT = dyn_cast<FunctionType>(Ty.getPointer()))
    if (TupleType *TT = dyn_cast<TupleType>(FT->Input.getPointer()))
      NumArguments = TT->Fields.size();
  
  if (VD->isOperator() && (NumArguments == 0 || NumArguments > 2)) {
    TC.diagnose(VD->getLocStart(), diag::invalid_arg_count_for_operator);
    Attrs.Infix = InfixData();
    // FIXME: Set the 'isError' bit on the decl.
    return;
  }
  
  // If the decl has an infix precedence specified, then it must be a function
  // whose input is a two element tuple.
  if (Attrs.isInfix() && NumArguments != 2) {
    TC.diagnose(Attrs.LSquareLoc, diag::invalid_infix_left_input);
    Attrs.Infix = InfixData();
    // FIXME: Set the 'isError' bit on the decl.
  }

  if (Attrs.isInfix() && !VD->isOperator()) {
    TC.diagnose(VD->getLocStart(), diag::infix_left_not_an_operator);
    Attrs.Infix = InfixData();
    // FIXME: Set the 'isError' bit on the decl.
  }

  // Only var and func decls can be infix.
  if (Attrs.isInfix() && !isa<VarDecl>(VD) && !isa<FuncDecl>(VD)) {
    TC.diagnose(VD->getLocStart(), diag::infix_left_invalid_on_decls);
    Attrs.Infix = InfixData();
  }

  if (VD->isOperator() && !VD->getAttrs().isInfix() && NumArguments != 1) {
    TC.diagnose(VD->getLocStart(), diag::binops_infix_left);
  }
}

bool DeclChecker::validateVarName(Type Ty, DeclVarName *Name) {
  // Check for a type specifier mismatch on this level.
  assert(Ty && "This lookup should never fail");
  
  // If this is a simple varname, then it matches any type, and we're done.
  if (Name->isSimple())
    return false;
  
  // If we're peering into an unresolved type, we can't analyze it yet.
  if (Ty->is<DependentType>()) return false;
  
  // If we have a single-element oneof (like a struct) then we allow matching
  // the struct elements with the tuple syntax.
  if (OneOfType *OOT = Ty->getAs<OneOfType>())
    if (OOT->hasSingleElement())
      Ty = OOT->getElement(0)->ArgumentType;
  
  // If we have a complex case, Ty must be a tuple and the name specifier must
  // have the correct number of elements.
  TupleType *AccessedTuple = Ty->getAs<TupleType>();
  if (AccessedTuple == 0) {
    TC.diagnose(Name->getLocation(), diag::name_matches_nontuple, Ty);
    return true;
  }
  
  // Verify the # elements line up.
  ArrayRef<DeclVarName *> Elements = Name->getElements();
  if (Elements.size() != AccessedTuple->Fields.size()) {
    TC.diagnose(Name->getLocation(), diag::varname_element_count_mismatch,
                Ty, AccessedTuple->Fields.size(), Elements.size());
    return true;
  }
  
  // Okay, everything looks good at this level, recurse.
  for (unsigned i = 0, e = Elements.size(); i != e; ++i) {
    if (validateVarName(AccessedTuple->Fields[i].Ty, Elements[i]))
      return true;
  }
  
  return false;
}

