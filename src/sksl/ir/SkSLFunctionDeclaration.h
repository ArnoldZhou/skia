/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SKSL_FUNCTIONDECLARATION
#define SKSL_FUNCTIONDECLARATION

#include "src/sksl/ir/SkSLExpression.h"
#include "src/sksl/ir/SkSLModifiers.h"
#include "src/sksl/ir/SkSLSymbol.h"
#include "src/sksl/ir/SkSLSymbolTable.h"
#include "src/sksl/ir/SkSLType.h"
#include "src/sksl/ir/SkSLVariable.h"

namespace SkSL {

/**
 * A function declaration (not a definition -- does not contain a body).
 */
struct FunctionDeclaration : public Symbol {
    FunctionDeclaration(int offset, Modifiers modifiers, StringFragment name,
                        std::vector<const Variable*> parameters, const Type& returnType)
    : INHERITED(offset, kFunctionDeclaration_Kind, std::move(name))
    , fDefined(false)
    , fBuiltin(false)
    , fModifiers(modifiers)
    , fParameters(std::move(parameters))
    , fReturnType(returnType) {}

    String declaration() const {
        String result = fReturnType.displayName() + " " + fName + "(";
        String separator;
        for (auto p : fParameters) {
            result += separator;
            separator = ", ";
            result += p->fName;
        }
        result += ")";
        return result;
    }

#ifdef SK_DEBUG
    String description() const override {
        return this->declaration();
    }
#endif

    bool matches(const FunctionDeclaration& f) const {
        if (fName != f.fName) {
            return false;
        }
        if (fParameters.size() != f.fParameters.size()) {
            return false;
        }
        for (size_t i = 0; i < fParameters.size(); i++) {
            if (fParameters[i]->fType != f.fParameters[i]->fType) {
                return false;
            }
        }
        return true;
    }

    /**
     * Determine the effective types of this function's parameters and return value when called with
     * the given arguments. This is relevant for functions with generic parameter types, where this
     * will collapse the generic types down into specific concrete types.
     *
     * Returns true if it was able to select a concrete set of types for the generic function, false
     * if there is no possible way this can match the argument types. Note that even a true return
     * does not guarantee that the function can be successfully called with those arguments, merely
     * indicates that an attempt should be made. If false is returned, the state of
     * outParameterTypes and outReturnType are undefined.
     */
    bool determineFinalTypes(const std::vector<std::unique_ptr<Expression>>& arguments,
                             std::vector<const Type*>* outParameterTypes,
                             const Type** outReturnType) const {
        SkASSERT(arguments.size() == fParameters.size());
        int genericIndex = -1;
        for (size_t i = 0; i < arguments.size(); i++) {
            if (fParameters[i]->fType.kind() == Type::kGeneric_Kind) {
                std::vector<const Type*> types = fParameters[i]->fType.coercibleTypes();
                if (genericIndex == -1) {
                    for (size_t j = 0; j < types.size(); j++) {
                        if (arguments[i]->fType.canCoerceTo(*types[j])) {
                            genericIndex = j;
                            break;
                        }
                    }
                    if (genericIndex == -1) {
                        return false;
                    }
                }
                outParameterTypes->push_back(types[genericIndex]);
            } else {
                outParameterTypes->push_back(&fParameters[i]->fType);
            }
        }
        if (fReturnType.kind() == Type::kGeneric_Kind) {
            if (genericIndex == -1) {
                return false;
            }
            *outReturnType = fReturnType.coercibleTypes()[genericIndex];
        } else {
            *outReturnType = &fReturnType;
        }
        return true;
    }

    mutable bool fDefined;
    bool fBuiltin;
    Modifiers fModifiers;
    const std::vector<const Variable*> fParameters;
    const Type& fReturnType;

    typedef Symbol INHERITED;
};

} // namespace

#endif
