// SPDX-License-Identifier: GPL-3.0-or-later
#include "belmont_pins.h"
#include <stdexcept>
#include <iostream>
void Check(bool b){if(!b)throw std::runtime_error("pin interaction contract");}
int main(){
 belmont_pins::Store s;Check(!s.Begin(0));s.Context("A",{0,0,100,100},true);
 Check(s.Begin(1));Check(!s.Capture(1,10,10));Check(s.Waiting());Check(s.Capture(2,10,20));
 Check(s.Picker()&&!s.Editing());Check(!s.Choose(9));Check(s.Choose(3));
 Check(s.All()[0]&&s.All()[0]->x==10&&s.All()[0]->y==20&&s.All()[0]->kind==3);
 Check(s.Select(0)&&s.Editing());s.Cancel();Check(bool(s.All()[0]));
 Check(s.Select(0));Check(s.Choose(8));Check(s.All()[0]->kind==8);
 s.Context("B",{0,0,100,100},true);Check(!s.Select(0));
 Check(s.Begin(2));Check(!s.Capture(3,NAN,2));Check(!s.Picker());
 Check(s.Begin(3));Check(!s.Capture(4,101,2));
 s.Context("A",{0,0,100,100},true);Check(s.Select(0));s.Context("A",{0,0,100,100},false);Check(!s.Remove());Check(bool(s.All()[0]));
 s.Context("A",{0,0,100,100},true);Check(s.Select(0));Check(s.Remove());Check(!s.All()[0]);
 for(unsigned n=0;n<32;++n){Check(s.Begin(10+n));Check(s.Capture(11+n,1,2));Check(s.Choose(n%9));}
 Check(s.Begin(99));Check(s.Capture(100,1,2));Check(!s.Choose(0));Check(s.Picker());s.Clear();Check(!s.All()[0]&&!s.Picker());
 Check(belmont_pins::NativeImage(0).find("1912900965121370566")!=std::string::npos);Check(belmont_pins::Image(9).empty());
 std::cout<<"pin store tests passed\n";
}
