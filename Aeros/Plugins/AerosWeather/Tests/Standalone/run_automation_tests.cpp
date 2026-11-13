// Runs the engine automation tests of Private/Tests/AerosSimulationTests.cpp without Unreal.
// Same test code as in the editor (Session Frontend > Automation > Aeros), compiled against the shim.

#include <cstdio>
#include <string>

struct FAutoTestBase
{
	int Fails = 0;
	bool TestTrue(const char* Name, bool bValue)
	{
		std::printf("  [%s] %s\n", bValue ? "PASS" : "FAIL", Name);
		if (!bValue) ++Fails;
		return bValue;
	}
	template <class A, class B> bool TestEqual(const char* Name, A Actual, B Expected) { return TestTrue(Name, Actual == Expected); }
};

#define WITH_DEV_AUTOMATION_TESTS 1
#define TEXT(x) x
#define IMPLEMENT_SIMPLE_AUTOMATION_TEST(Cls, Name, Flags) struct Cls : FAutoTestBase { bool RunTest(const std::string& Parameters); };
#define FString std::string
namespace EAutomationTestFlags { enum { EditorContext = 1, EngineFilter = 2 }; }

#include "Tests/AerosSimulationTests.cpp"
#undef FString

int main()
{
	int Fails = 0;
	{ FAerosPhysicsTest T;   std::printf("Physics\n");   T.RunTest(""); Fails += T.Fails; }
	{ FAerosSupercellTest T; std::printf("Supercell\n"); T.RunTest(""); Fails += T.Fails; }
	{ FAerosHazardTest T;    std::printf("Disasters\n"); T.RunTest(""); Fails += T.Fails; }
	{ FAerosCloudVolumeTest T; std::printf("Cloud voxels\n"); T.RunTest(""); Fails += T.Fails; }
	std::printf("failures: %d\n", Fails);
	return Fails == 0 ? 0 : 1;
}
