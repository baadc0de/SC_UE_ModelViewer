// SCBridge.Fx.ExpressionValidation: the engine fork's stateless expression validation (WP-E1 E1-6 review), which the emitter build
// runs before it builds a spawn or loop expression (an invalid one becomes the distribution's constant, with a warning).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "NiagaraTypes.h"
#include "Stateless/NiagaraStatelessExpression.h"
#include "Stateless/Expressions/NiagaraStatelessExpressionFloat.h"
#include "Stateless/Expressions/NiagaraStatelessExpressionVec3.h"
#include "NiagaraParameterStore.h"
#include "SCFxBuildCommandlet.h"
#include <cmath>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFxExpressionValidationTest, "SCBridge.Fx.ExpressionValidation",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFxExpressionValidationTest::RunTest(const FString&)
{
    const FNiagaraTypeDefinition Float = FNiagaraTypeDefinition::GetFloatDef();
    FString Error;
    const auto Bind = [](FName Name) {
        FNiagaraStatelessExpressionFloatBinding B;
        B.A = Name;
        return FInstancedStruct::Make(B);
    };
    const auto Mul = [](FInstancedStruct A, FInstancedStruct B) {
        FNiagaraStatelessExpressionFloatMultiply M;
        M.A = MoveTemp(A);
        M.B = MoveTemp(B);
        return FInstancedStruct::Make(M);
    };
    FNiagaraStatelessExpressionFloatConstant Two;
    Two.A = 2.f;
    TestTrue(TEXT("a bound rate x 2 is valid"), FNiagaraStatelessExpression::ValidateTree(Mul(Bind(TEXT("User.SC.BeamLength")), FInstancedStruct::Make(Two)), Float, Error));
    TestFalse(TEXT("an empty root is not"), FNiagaraStatelessExpression::ValidateTree(FInstancedStruct(), Float, Error));
    TestFalse(TEXT("a binding without a name (NAME_None leaf: offset -4) is not"), FNiagaraStatelessExpression::ValidateTree(Mul(Bind(NAME_None), FInstancedStruct::Make(Two)), Float, Error));
    TestTrue(TEXT("the error names the leaf"), Error.Contains(TEXT("binds no parameter")));
    TestFalse(TEXT("an empty operand is not"), FNiagaraStatelessExpression::ValidateTree(Mul(FInstancedStruct(), FInstancedStruct::Make(Two)), Float, Error));
    FNiagaraStatelessExpressionVec3Constant Vec;
    TestFalse(TEXT("a vec3 root for a float distribution is not"), FNiagaraStatelessExpression::ValidateTree(FInstancedStruct::Make(Vec), Float, Error));
    // Review fixes 3 (Astra A2): the operand-type and concrete-node checks hold in every build (not only with editor-only metadata): a
    // vec3 operand of a float multiply, a bare typed base (no evaluator: checkNoEntry) as the root or as an operand.
    TestFalse(TEXT("a vec3 operand of a float multiply is not"), FNiagaraStatelessExpression::ValidateTree(Mul(FInstancedStruct::Make(Vec), FInstancedStruct::Make(Two)), Float, Error));
    TestFalse(TEXT("a bare float base root is not"), FNiagaraStatelessExpression::ValidateTree(FInstancedStruct::Make(FNiagaraStatelessExpressionFloat()), Float, Error));
    TestFalse(TEXT("a bare float base operand is not"), FNiagaraStatelessExpression::ValidateTree(Mul(FInstancedStruct::Make(FNiagaraStatelessExpressionFloat()), FInstancedStruct::Make(Two)), Float, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFxStepExpressionTest, "SCBridge.Fx.StepExpression",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFxStepExpressionTest::RunTest(const FString&)
{
    // Review fixes 3 (Terra T5 / Astra A7): the builder's expression for a strength curve with a step, evaluated by the engine at the
    // key and the next float above it (a denormal at 0): the comparison expression is strict there, as the rule (SCFxRules) is.
    FNiagaraParameterStore Store;
    const auto Value = [&Store](const FInstancedStruct& E) {
        float V = -1.f;
        E.Get<FNiagaraStatelessExpression>().Evaluate(FNiagaraStatelessExpression::FEvaluateContext(Store), &V);
        return V;
    };
    const auto Const = [](float X) {
        FNiagaraStatelessExpressionFloatConstant C;
        C.A = X;
        return FInstancedStruct::Make(C);
    };
    for (const float T : {0.0f, 0.25f, 0.5f})
    {
        const TArray<FVector2D> Keys = {FVector2D(T, 0.25), FVector2D(T, 0.75), FVector2D(1.0, 0.75)};
        const float Next = std::nextafter(T, FLT_MAX);
        FString Error;
        TestTrue(FString::Printf(TEXT("step at %g: the built tree is valid"), T),
                 FNiagaraStatelessExpression::ValidateTree(SCFxPiecewiseLinearExpression(Keys, Const(T)), FNiagaraTypeDefinition::GetFloatDef(), Error));
        TestEqual(FString::Printf(TEXT("step at %g: the earlier value at the key"), T), Value(SCFxPiecewiseLinearExpression(Keys, Const(T))), 0.25f, 1e-7f);
        TestEqual(FString::Printf(TEXT("step at %g: the later value at the next float %g"), T, Next), Value(SCFxPiecewiseLinearExpression(Keys, Const(Next))), 0.75f, 1e-7f);
        TestEqual(FString::Printf(TEXT("step at %g: the later value at 1"), T), Value(SCFxPiecewiseLinearExpression(Keys, Const(1.f))), 0.75f, 1e-7f);
    }
    FNiagaraStatelessExpressionFloatGreaterThan G;
    G.A = Const(1.f);
    G.B = Const(0.f);
    TestEqual(TEXT("greater than: 1 > 0"), Value(FInstancedStruct::Make(G)), 1.f);
    G.A = Const(0.f);
    TestEqual(TEXT("greater than: 0 > 0 is 0"), Value(FInstancedStruct::Make(G)), 0.f);
    return true;
}
#endif
