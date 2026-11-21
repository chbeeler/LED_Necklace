#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
extern "C" bool updateButtons(bool,bool,uint32_t);
static std::vector<std::string> reports;
extern "C" void reportAeqMenu(const char *name, uint16_t value) {
    reports.push_back(std::string(name)+": "+std::to_string(value));
}
static uint32_t now;
static void tick(bool plus, bool minus, unsigned duration) {
    for(unsigned i=0;i<duration;i+=16) {now+=16;updateButtons(plus,minus,now);}
}
static void press(bool plus, unsigned duration) {tick(plus,!plus,duration);tick(false,false,64);}
int main() {
    tick(false,false,64); press(true,100); assert(reports.empty());
    press(false,1100); assert(reports.size()==1 && reports.back()=="SENS: 50");
    tick(false,false,1000); assert(reports.size()==1);
    press(true,100); assert(reports.size()==2 && reports.back()=="SENS: 55");
    press(false,100); assert(reports.size()==3 && reports.back()=="SENS: 50");
    press(false,1100); assert(reports.size()==4 && reports.back()=="GLOW: 60");
    press(true,100); assert(reports.back()=="GLOW: 70");
    press(false,1100); assert(reports.back()=="IDLE: 4");
    press(true,100); assert(reports.back()=="IDLE: 5");
    auto count=reports.size(); press(true,1100);tick(false,false,31000);
    press(true,100); assert(reports.size()==count);
}
