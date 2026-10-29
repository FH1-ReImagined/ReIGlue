#include <rex/ui/vulkan/hdr_format.h>
#include <array>
#include <cassert>
using namespace rex::ui::vulkan;
int main() {
  struct Format { int format, colorSpace; };
  constexpr Format sdr{1,1};
  constexpr Format hdr{2,2};
  const std::array candidates{sdr,hdr};
  auto choice = ChooseHdrFormat(candidates,sdr,true,true,hdr);
  assert(choice.active && choice.available && choice.format.format == hdr.format);
  choice = ChooseHdrFormat(candidates,sdr,true,false,hdr);
  assert(!choice.active && choice.available && choice.format.format == sdr.format);
  choice = ChooseHdrFormat(candidates,sdr,false,true,hdr);
  assert(!choice.active && !choice.available && choice.format.format == sdr.format);
  const std::array wrong{Format{hdr.format,sdr.colorSpace},
                         Format{sdr.format,hdr.colorSpace}};
  choice = ChooseHdrFormat(wrong,sdr,true,true,hdr);
  assert(!choice.active && !choice.available && choice.format.format == sdr.format);
  choice = ChooseHdrFormat(std::array<Format,0>{},sdr,true,true,hdr);
  assert(!choice.active && !choice.available);
}
