"""Exercise the production charset switch against SDK language/type constants."""
from pathlib import Path
import re,subprocess,sys,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/platform/psp/psp_osk.cpp').read_text()
a=source.index('    switch (charset) {');b=source.index('    g_data.lines',a)
switch=source[a:b]
# Constants are copied directly from the installed SDK's enums by the caller.
sdk=Path(sys.argv[2]);header=(sdk/'psputility_osk.h').read_text()
constants='\n'.join('constexpr int '+name+'='+value+';' for name,value in re.findall(r'(PSP_UTILITY_OSK_(?:LANGUAGE|INPUTTYPE)_\w+)\s*=\s*(0x[0-9a-fA-F]+)',header))
cpp=r'''
#include "platform/psp/psp_osk.hpp"
#include <cassert>
#include <cstdio>
using namespace wowee::platform::psp;
struct {int language=-1,inputtype=-1;} g_data;
CONSTANTS
void configure(OskCharset charset){ SWITCH }
int main(){
 for(auto kind:{OskCharset::Any,OskCharset::Text}){
  configure(kind);assert(g_data.language==PSP_UTILITY_OSK_LANGUAGE_DEFAULT);
  assert(g_data.inputtype==PSP_UTILITY_OSK_INPUTTYPE_ALL);
 }
 configure(OskCharset::Digits);assert(g_data.inputtype==PSP_UTILITY_OSK_INPUTTYPE_LATIN_DIGIT);
 configure(OskCharset::LatinLetters);assert(g_data.language==PSP_UTILITY_OSK_LANGUAGE_ENGLISH);
 configure(OskCharset::Cyrillic);assert(g_data.language==PSP_UTILITY_OSK_LANGUAGE_RUSSIAN);
 std::puts("OSK language passed: default firmware-language layout, unrestricted localized text, explicit field restrictions retained.");
}
'''.replace('CONSTANTS',constants).replace('SWITCH',switch)
compiler=sys.argv[1]
with tempfile.TemporaryDirectory(prefix='wowee-osk-') as temp:
 folder=Path(temp);src=folder/'osk.cpp';exe=folder/'osk.exe';src.write_text(cpp)
 if Path(compiler).stem.lower()=='cl':args=[compiler,'/nologo','/std:c++20','/EHsc','/I'+str(root/'include'),str(src),'/Fe:'+str(exe),'/Fo:'+str(folder/'osk.obj')]
 else:args=[compiler,'-std=c++20','-I'+str(root/'include'),str(src),'-o',str(exe)]
 subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True)
