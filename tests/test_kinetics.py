"""Touch kinetics: finger speed, the coast after a flick, the rubber band and glide, the page under an edge gesture."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("clang++"), "Requires the host C++ compiler")
class KineticsTests(unittest.TestCase):
    def test_flick_coasts_bounces_glides_and_the_page_follows(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "kinetics.cpp"
            source.write_text(
                r'''#include "launcher/kinetics.hpp"
#include "launcher/carousel.hpp"
#include <cassert>
#include <cmath>
#include <utility>
using namespace rgds;
int main(){
 // A finger that stops before lifting has no speed; a moving one has its recent speed.
 FingerSpeed still;for(unsigned t=0;t<=100;t+=10)still.add(t,t*2.0);still.add(300,200);
 assert(still.estimate(300)==0);
 FingerSpeed quick;for(unsigned t=0;t<=96;t+=16)quick.add(t,t*1.5);
 assert(std::abs(quick.estimate(100)-1.5)<1e-9);
 // A flick: the list coasts on after the lift, slows, and ends at rest on a whole pixel inside its bounds.
 ScrollMotion list;list.begin(0,100);int scroll=100;
 for(unsigned t=16;t<=96;t+=16)scroll=list.drag(t,100+t*1.2,0,800);
 assert(scroll==215 && std::abs(list.stretch(scroll))<1);
 list.release(100,0,800);assert(list.coasting);
 int previous=scroll;bool advanced=false;unsigned ended=0;
 for(unsigned t=116;t<2000 && list.active();t+=16){list.step(t,scroll,0,800);assert(scroll>=previous);advanced|=scroll>previous;previous=scroll;ended=t;}
 assert(advanced && !list.active() && scroll>300 && scroll<800 && ended<1200 && list.stretch(scroll)==0);
 // Pulled past the top: the band gives less and less, and springs back to the end once released.
 ScrollMotion band;band.begin(0,0);
 band.drag(20,-60,0,800);double small=band.stretch(0);band.drag(40,-240,0,800);double big=band.stretch(0);
 assert(small<0 && big<small && -big<ScrollMotion::extent && -big<4*-small);
 band.release(1000,0,800);assert(band.coasting);scroll=0;
 for(unsigned t=1016;t<3000 && band.active();t+=16){band.step(t,scroll,0,800);assert(scroll==0);assert(band.stretch(0)<=1e-6);}
 assert(!band.active() && band.stretch(0)==0);
 // A fast flick into the end bounces a little past it and settles exactly on it.
 ScrollMotion hit;hit.begin(0,700);for(unsigned t=16;t<=80;t+=16)hit.drag(t,700+t*2.5,0,800);
 hit.release(80,0,800);scroll=0;double deepest=0;
 for(unsigned t=96;t<3000 && hit.active();t+=16){hit.step(t,scroll,0,800);deepest=std::max(deepest,hit.stretch(scroll));}
 assert(scroll==800 && deepest>2 && deepest<ScrollMotion::extent && hit.stretch(800)==0);
 // A hard flick runs much further than a firm one, and flicking again during the coast adds to it.
 auto run=[](double speed,int start,int max){ScrollMotion m;m.begin(0,start);for(unsigned t=16;t<=80;t+=16)m.drag(t,start+t*speed,0,max);
  m.release(80,0,max);int at=start;for(unsigned t=96;t<20000 && m.active();t+=16)m.step(t,at,0,max);assert(!m.active());return at;};
 const int firm=run(1.8,0,100000),hard=run(4.5,0,100000);assert(firm>400 && firm<900 && hard>3*firm);
 ScrollMotion twice;twice.begin(0,0);int at=0;for(unsigned t=16;t<=80;t+=16)at=twice.drag(t,t*4.5,0,100000);twice.release(80,0,100000);
 const double first_speed=twice.velocity;twice.step(120,at,0,100000);const double running=twice.velocity;twice.stop();
 twice.begin(130,at);assert(twice.carried==running);for(unsigned t=146;t<=210;t+=16)twice.drag(t,at+(t-130)*4.5,0,100000);twice.release(210,0,100000);
 assert(twice.velocity>first_speed && twice.velocity<=ScrollMotion::top_speed);
 // A touch that stops the coast and rests there does not inherit its speed.
 ScrollMotion stopped;stopped.begin(0,0);for(unsigned t=16;t<=80;t+=16)stopped.drag(t,t*4.5,0,100000);stopped.release(80,0,100000);stopped.stop();
 stopped.begin(100,500);stopped.drag(116,500,0,100000);stopped.release(1000,0,100000);assert(stopped.velocity==0);
 // The fastest flick still only bounces a little at the end.
 ScrollMotion wall;wall.begin(0,0);for(unsigned t=16;t<=80;t+=16)wall.drag(t,t*6.0,0,1500);wall.release(80,0,1500);at=0;double beyond=0;
 for(unsigned t=96;t<5000 && wall.active();t+=16){wall.step(t,at,0,1500);beyond=std::max(beyond,wall.stretch(at));}
 assert(at==1500 && beyond>2 && beyond<ScrollMotion::extent && wall.stretch(1500)==0);
 // A focus change glides: the drawing starts where it was, overshoots a little and settles on the new scroll.
 ScrollMotion glide;glide.glide(0,0,164,true);assert(glide.settling && glide.stretch(164)==-164);
 double over=0;scroll=164;
 for(unsigned t=16;t<2000 && glide.active();t+=16){glide.step(t,scroll,0,800);assert(scroll==164);over=std::max(over,glide.stretch(164));}
 assert(!glide.active() && over>1 && over<24 && glide.stretch(164)==0);
 ScrollMotion plain;plain.glide(0,0,164,false);assert(!plain.active() && plain.stretch(164)==0);
 // The page under a Home pull lifts and shrinks, departs smaller and fading, or springs back whole.
 PageDrag page;assert(!page.active());
 page.follow(PageDrag::Kind::Home,2);assert(page.holding() && page.dy<0 && page.scale<1 && page.scale>.85);
 page.follow(PageDrag::Kind::Home,72);double lifted=page.dy,shrunk=page.scale;
 page.follow(PageDrag::Kind::Home,300);assert(page.dy<lifted && page.dy>lifted-60 && page.scale<shrunk && page.scale>.8);
 page.depart(1000);assert(page.departing());bool faded=false;
 for(unsigned t=1016;t<2000 && page.active();t+=16){page.update(t,1.0);if(page.active())assert(page.alpha>=0 && page.alpha<=1 && page.scale<=shrunk+1e-9);faded|=page.alpha<.5;}
 assert(faded && !page.active() && page.scale==1 && page.alpha==1);
 page.follow(PageDrag::Kind::Back,40);assert(page.dx>0 && page.dy==0);page.settle(3000);assert(page.returning());
 for(unsigned t=3016;t<5000 && page.active();t+=16)page.update(t,1.0);
 assert(!page.active() && page.dx==0 && page.scale==1);
 // A relaxed pace takes longer than the normal one.
 PageDrag slow;slow.follow(PageDrag::Kind::Home,72);slow.depart(0);slow.update(250,2.8);assert(slow.active());
 PageDrag normal;normal.follow(PageDrag::Kind::Home,72);normal.depart(0);normal.update(320,1.0);assert(!normal.active());
 // The carousel: an ordinary swipe settles on the nearest item, only a hard flick coasts on, further the harder.
 Carousel soft;soft.reset(2);soft.down(320,205,false,0);
 for(unsigned t=16;t<=80;t+=16){soft.drag(320-int(t*1.6),20);soft.sample(t);}
 assert(soft.flick_target(80,20)==size_t(std::llround(soft.position)));
 auto flick=[](double speed){Carousel c;c.reset(2);c.down(320,205,false,0);for(unsigned t=16;t<=64;t+=16){c.drag(320-int(t*speed),40);c.sample(t);}
  return std::make_pair(size_t(std::llround(c.position)),c.flick_target(64,40));};
 auto hard4=flick(4),hard7=flick(7);assert(hard4.second>=hard4.first+3 && hard7.second>=hard7.first+8 && hard7.second-hard7.first>hard4.second-hard4.first);
 Carousel rest;rest.reset(2);rest.down(320,205,false,0);rest.drag(250,10);rest.sample(16);rest.sample(400);
 assert(rest.flick_target(400,10)==size_t(std::llround(rest.position)));
 // Longer travels take longer, and every tween still reaches its target.
 Carousel far;far.reset(0);far.select(1,0);unsigned one=far.duration;far.select(4,0);assert(far.duration==310 && far.duration>one);
 far.select(30,0);assert(far.duration==510);far.select(5,0);
 far.update(far.duration);assert(!far.moving && far.position==5);
}
'''
            )
            binary = root / "kinetics"
            subprocess.run(
                [
                    "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"),
                    str(source), "-o",
                    str(binary)
                ],
                check=True,
                capture_output=True,
                text=True
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
