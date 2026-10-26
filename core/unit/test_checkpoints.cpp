#include <string.h>
#include "../src/features/checkpoints.h"
#include "unit.h"
namespace {
bool ParseText(const char* source, fdcore::checkpoints::Set* set) {
    char text[fdcore::checkpoints::kTextMax];
    if (strlen(source) >= sizeof(text)) return false;
    strcpy(text, source); return fdcore::checkpoints::Parse(text, set);
}
}
void TestCheckpoints() {
    using namespace fdcore::checkpoints;
    Set set{};
    const char* valid = "FDNY_CHECKPOINTS_V1|training|2\n0|0|0|100|100|100|Door\n200|-50|200|300|100|300|Stairs\n";
    CHECK(ParseText(valid, &set)); CHECK(set.count == 2); CHECK(Equal(set.map, "training"));
    CHECK(Equal(set.points[0].label, "Door")); CHECK(set.points[1].min[1] == -50);
    CHECK(ParseText("FDNY_CHECKPOINTS_V1|training|1\r\n0|0|0|8|8|8|Min\r\n", &set));
    const char* bad[] = {
        "", "FDNYCP1|training|1\n0|0|0|100|100|100|Old format\n",
        "FDNY_CHECKPOINTS_V1|../bad|1\n0|0|0|100|100|100|Path\n",
        "FDNY_CHECKPOINTS_V1|training|\n", "FDNY_CHECKPOINTS_V1|training|33\n",
        "FDNY_CHECKPOINTS_V1|training|1\n0|0|0|100|100|100|",
        "FDNY_CHECKPOINTS_V1|training|1\n0|0|0|100|100|100|bad(text)\n",
        "FDNY_CHECKPOINTS_V1|training|1\n0|0|0|100|100|100|extra|field\n",
        "FDNY_CHECKPOINTS_V1|training|1\nNaN|0|0|100|100|100|Bad\n",
        "FDNY_CHECKPOINTS_V1|training|1\n0|0|0|0|100|100|Zero\n",
        "FDNY_CHECKPOINTS_V1|training|1\n0|0|0|20000|100|100|Huge\n",
        "FDNY_CHECKPOINTS_V1|training|2\n0|0|0|100|100|100|Truncated\n",
        "FDNY_CHECKPOINTS_V1|training|1\n0|0|0|100|100|100|Good\nextra\n"
    };
    for (const char* text : bad) CHECK(!ParseText(text, &set));
    float number = 0; CHECK(Number("-1268.5", &number) && number == -1268.5f);
    CHECK(!Number("1e3", &number)); CHECK(!Number("inf", &number)); CHECK(!Number("110001", &number));
    CHECK(ParseText(valid, &set)); const float lo[] = {-30,-30,-30}, hi[] = {30,85,30};
    CHECK(Inside(set.points[0],lo,hi)); CHECK(!Inside(set.points[1],lo,hi));
    bool armed = false, hit = false;
    CHECK(!Contact(true,true,armed,hit)); // at spawn: must leave first
    CHECK(!Contact(false,true,armed,hit) && armed);
    CHECK(!Contact(true,false,armed,hit) && !hit); // disabled
    CHECK(Contact(true,true,armed,hit) && hit);
    CHECK(!Contact(false,true,armed,hit)); CHECK(!Contact(true,true,armed,hit));
    // Ray selection chooses the closest box, handles parallel axes and empty clicks.
    const float origin[] = {-50,50,50}, right[] = {1,0,0}, away[] = {-1,0,0}, up[] = {0,1,0};
    CHECK(Pick(set,origin,right) == 0); CHECK(Pick(set,origin,away) == -1); CHECK(Pick(set,origin,up) == -1);
    const float inside[] = {50,50,50}; CHECK(Pick(set,inside,right) == 0);
    Point p = set.points[0];
    CHECK(Adjust(&p,0,8,false) && p.min[0] == 8 && p.max[0] == 108);
    CHECK(Adjust(&p,1,-8,true) && p.min[1] == 4 && p.max[1] == 96);
    const Point before = p;
    CHECK(!Adjust(&p,1,-100,true) && p.min[1] == before.min[1] && p.max[1] == before.max[1]);
    CHECK(!Adjust(&p,3,8,false)); CHECK(!Adjust(&p,0,200000,false));
    const float plane[] = {0,0,100}, normal[] = {0,0,1}, eye[] = {0,0,0}, ray[] = {.5f,.25f,1};
    float hitPoint[3];
    CHECK(RayPlane(eye,ray,plane,normal,hitPoint) && hitPoint[0] == 50 && hitPoint[1] == 25 && hitPoint[2] == 100);
    const float parallel[] = {1,0,0}, behind[] = {0,0,-1};
    CHECK(!RayPlane(eye,parallel,plane,normal,hitPoint)); CHECK(!RayPlane(eye,behind,plane,normal,hitPoint));
    Point dragged{}; const float delta[] = {10,-20,30};
    CHECK(DragPoint(before,delta,-1,0,&dragged) && dragged.min[0] == before.min[0]+10 && dragged.max[2] == before.max[2]+30);
    CHECK(DragPoint(before,delta,0,0,&dragged) && dragged.min[0] == before.min[0]+10 && dragged.min[1] == before.min[1]);
    CHECK(DragPoint(before,delta,2,1,&dragged) && dragged.max[2] == before.max[2]+30 && dragged.min[2] == before.min[2]);
    CHECK(DragPoint(before,delta,1,-1,&dragged) && dragged.min[1] == before.min[1]-20 && dragged.max[1] == before.max[1]);
    const float extreme[] = {-200000,200000,200000};
    CHECK(DragPoint(before,extreme,0,1,&dragged) && dragged.max[0]-dragged.min[0] == 8);
    CHECK(DragPoint(before,extreme,1,-1,&dragged) && dragged.max[1]-dragged.min[1] == 8);
    CHECK(DragPoint(before,extreme,2,1,&dragged) && dragged.max[2]-dragged.min[2] == 10000);
    CHECK(!DragPoint(before,extreme,-1,0,&dragged));
    char label[81];
    AlphabetLabel(0,label); CHECK(Equal(label,"Checkpoint A"));
    AlphabetLabel(25,label); CHECK(Equal(label,"Checkpoint Z"));
    AlphabetLabel(26,label); CHECK(Equal(label,"Checkpoint AA"));
    AlphabetLabel(31,label); CHECK(Equal(label,"Checkpoint AF"));
    char saved[kTextMax]; Set restored{};
    set.points[0].min[0] = -12.125f; set.points[0].max[1] = 100.0001f;
    CHECK(Write(set,saved,sizeof(saved)) > 0); CHECK(ParseText(saved,&restored));
    CHECK(Equal(restored.points[1].label,"Stairs") && restored.points[0].min[0] == -12.125f);
    CHECK(restored.points[0].max[1] == set.points[0].max[1]);
    CHECK(!Write(set,saved,10));
    set.points[0].label[0] = '|'; CHECK(!Write(set,saved,sizeof(saved)));
    set.count = 0; CHECK(Write(set,saved,sizeof(saved)) > 0);
    CHECK(ParseText(saved,&restored) && restored.count == 0);
    const auto time = Elapsed(1000, 15345); CHECK(time.minutes == 0 && time.seconds == 14 && time.millis == 345);
    const auto minute = Elapsed(0, 61001); CHECK(minute.minutes == 1 && minute.seconds == 1 && minute.millis == 1);
    const auto wrap = Elapsed(0xfffffff0u, 0x10); CHECK(wrap.minutes == 0 && wrap.seconds == 0 && wrap.millis == 32);
}
