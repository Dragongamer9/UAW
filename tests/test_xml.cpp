// Unit tests for the tolerant XML parser and the game object database.
#include "alamo/gamedata.hpp"
#include "alamo/sfx.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace alamo;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

static void testParser() {
    auto d = parseXml("<?xml version=\"1.0\"?>\n<Root><A name='x'> hi &amp; bye </A><B/><C>1</C></Root>");
    const XmlNode* r = d.root.child("root");
    CHECK(r);
    CHECK(r && r->child("a") && r->child("A")->text == "hi & bye");
    CHECK(r && *r->child("A")->attr("NAME") == "x");
    CHECK(r && r->child("B") && r->child("B")->children.empty());
    CHECK(d.warnings.empty());

    // Fragments with several roots (include files).
    auto f = parseXml("<A>1</A>\n<!-- note -->\n<B>2</B>");
    CHECK(f.root.children.size() == 2);

    // Mismatched close tag recovers instead of failing.
    auto m = parseXml("<R><X><Y>v</X><Z>2</Z></R>");
    const XmlNode* r2 = m.root.child("R");
    CHECK(r2 && r2->child("Z") && r2->child("Z")->text == "2");
    CHECK(!m.warnings.empty());

    // INCLUDE directives splice fragments in place, recursively.
    auto inc = parseXml("<R><!--INCLUDE \".\\Data\\a.xml\" --><End/></R>", [](std::string_view p) -> std::optional<std::string> {
        if (p == ".\\Data\\a.xml") return std::string("<A>1</A><!--INCLUDE \"b\" -->");
        if (p == "b") return std::string("<B>2</B>");
        return std::nullopt;
    });
    const XmlNode* r3 = inc.root.child("R");
    CHECK(r3 && r3->children.size() == 3);
    CHECK(r3 && r3->child("B") && r3->child("B")->text == "2");

    // Self-including file stops at the depth limit instead of recursing forever.
    auto loop = parseXml("<R><!--INCLUDE \"self\" --></R>", [](std::string_view) -> std::optional<std::string> {
        return std::string("<X/><!--INCLUDE \"self\" -->");
    });
    CHECK(!loop.warnings.empty());

    // Garbage must not crash.
    parseXml("<<<>>></ <a b=\"unterminated");
    parseXml("");
}

static void testDatabase() {
    fs::path root = fs::temp_directory_path() / "uaw_native_test_db";
    fs::remove_all(root);
    write(root / "Data/MegaFiles.xml", "<Mega_Files></Mega_Files>");
    write(root / "Data/XML/GameObjectFiles.xml",
          "<Game_Object_Files><File> Units/Test.xml </File><!-- <File>Gone.xml</File> --></Game_Object_Files>");
    write(root / "Data/XML/Includes/Sell.xml", "<Effect_Generator Name=\"Sell\"><Duration>2</Duration></Effect_Generator>");
    write(root / "Data/XML/Units/Test.xml", R"(<?xml version="1.0"?>
<Test_XML>
  <X10_Alien_Units Name="Grunt">
    <Tactical_Health> 300 </Tactical_Health>
    <Max_Speed>1.0f</Max_Speed>
    <Affiliation> Alien, Alien_ZM06 </Affiliation>
    <BehaviorType Name="ProductionBehaviorType">
      <Build_Cost_Credits> 275 </Build_Cost_Credits>
    </BehaviorType>
    <BehaviorType name="BuildableObjectBehaviorType">
      <Tactical_Build_Cost_Multiplayer> 300 </Tactical_Build_Cost_Multiplayer>
    </BehaviorType>
    <Effect_Generator_Data SubObjectList="Yes">
      <!--INCLUDE ".\Data\XML\Includes\Sell.xml" -->
    </Effect_Generator_Data>
  </X10_Alien_Units>
  <X10_Alien_Units Name="Grunt_Elite">
    <Variant_Of_Existing_Type> Grunt </Variant_Of_Existing_Type>
    <Tactical_Health> 450 </Tactical_Health>
  </X10_Alien_Units>
  <X10_Alien_Units Name="Orphan">
    <Variant_Of_Existing_Type>DoesNotExist</Variant_Of_Existing_Type>
  </X10_Alien_Units>
</Test_XML>)");

    MegVfs vfs;
    auto missing = mountGame(vfs, root);
    CHECK(missing.empty());
    GameDatabase db;
    db.load(vfs);
    CHECK(db.objects().size() == 3);

    const GameObjectType* g = db.find("grunt");
    CHECK(g);
    if (g) {
        CHECK(g->number("Tactical_Health") == 300.0);
        CHECK(g->number("Max_Speed") == 1.0);
        CHECK(g->number("Build_Cost_Credits") == 275.0);
        CHECK(g->get("BehaviorType[ProductionBehaviorType].Build_Cost_Credits") != nullptr);
        CHECK(g->get("Effect_Generator_Data.Effect_Generator[Sell].Duration") != nullptr);
    }
    const GameObjectType* e = db.find("Grunt_Elite");
    CHECK(e);
    if (e) {
        CHECK(e->variantOf == "Grunt");
        CHECK(e->number("Tactical_Health") == 450.0);              // overridden
        CHECK(e->number("Tactical_Build_Cost_Multiplayer") == 300.0); // inherited
    }
    bool sawOrphan = false;
    for (const auto& w : db.warnings()) sawOrphan |= w.find("DoesNotExist") != std::string::npos;
    CHECK(sawOrphan);
    fs::remove_all(root);
}

static void testSfx() {
    SfxLibrary lib;
    lib.parse(R"(<SFXEvents>
      <SFXEvent Name="Preset_EXP"><Is_Preset>Yes</Is_Preset><Is_3D>Yes</Is_3D><Is_2D>No</Is_2D>
        <Max_Instances>3</Max_Instances><Volume_Saturation_Distance>400</Volume_Saturation_Distance>
        <Min_Volume>80</Min_Volume><Max_Volume>90</Max_Volume></SFXEvent>
      <SFXEvent Name="Boom"><Use_Preset>Preset_EXP</Use_Preset><Samples>EXP_A.wav EXP_B.wav</Samples>
        <Max_Volume>95</Max_Volume></SFXEvent>
      <SFXEvent Name="Click"><Is_2D>Yes</Is_2D><Is_GUI>Yes</Is_GUI><Samples>GUI_Click.wav</Samples></SFXEvent>
    </SFXEvents>)");
    const SfxEvent* b = lib.find("boom");
    CHECK(b && b->samples.size() == 2 && b->samples[1] == "EXP_B.wav");
    CHECK(b && b->is3D && b->maxInstances == 3 && b->saturationDistance == 400); // from the preset
    CHECK(b && b->minVolume == 80 && b->maxVolume == 95);                        // own field wins
    const SfxEvent* c = lib.find("Click");
    CHECK(c && !c->is3D && c->isGUI);
    CHECK(!lib.find("Nothing"));
}

int main() {
    testParser();
    testDatabase();
    testSfx();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all XML/gamedata tests passed\n");
    return 0;
}
