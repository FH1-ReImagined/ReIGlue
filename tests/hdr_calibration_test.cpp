#include <rex/ui/hdr_calibration.h>
#include <cassert>
#include <cmath>
using namespace rex::ui;
int main() {
  constexpr float width=1280, height=720, cell=180, left=280, top=90;
  const auto pixel = [&](int mode,float x,float y) {
    return HdrCalibrationSignal(mode,x,y,width,height,2.5f,12.5f);
  };
  assert(pixel(0,640,360) < 0 && pixel(1,0,0) < 0 && pixel(4,640,360) < 0);
  assert(HdrCalibrationSignal(1,0,0,0,0,2.5f,12.5f) < 0);
  float previous_peak=0;
  for (int row=0; row<3; ++row) {
    for (int column=0; column<4; ++column) {
      const float x=left+(column+.5f)*cell,y=top+(row+.5f)*cell;
      const int index=row*4+column;
      const float black=pixel(1,x,y);
      assert(black >= 0 && black <= 1.0f/80);
      if (!index) assert(black == 0);
      if (index == 11) assert(black == 1.0f/80);
      // A neighboring checker cell is absolute black, not gamma-encoded gray.
      assert(pixel(1,x+cell/8,y) == 0);
      const float paper=pixel(2,x,y);
      const float expected=column==0?.18f:column==1?.5f:column==2?.75f:1;
      assert(paper == 2.5f*expected);
      const float ring=pixel(2,left+(column+.05f)*cell,y);
      assert(ring == (row==0?0:row==1?.18f*2.5f:2.5f));
      const float peak=pixel(3,x,y);
      assert(peak > previous_peak && peak <= 12.5f);
      assert(pixel(3,left+(column+.05f)*cell,y) == 12.5f);
      previous_peak=peak;
    }
  }
  assert(previous_peak == 12.5f);
}
