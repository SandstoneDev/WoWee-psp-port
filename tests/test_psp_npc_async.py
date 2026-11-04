from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
compiler=sys.argv[1] if len(sys.argv)>1 else 'c++'
with tempfile.TemporaryDirectory(prefix='wowee-npc-async-') as temp:
    folder=Path(temp);worker=folder/'worker.cpp';exe=folder/'async.exe'
    # Track every production buffer release to catch leaks/double frees. The
    # kernel/file substitutes assert that all lookups and reads run off-main.
    worker.write_text((root/'src/platform/psp/psp_npc_asset_loader.cpp').read_text().replace('std::free(', 'npcTestFree('))
    if Path(compiler).stem.lower()=='cl':
        args=[compiler,'/nologo','/std:c++20','/EHsc','/O2','/DWOWEE_TEST_NPC_ASYNC=1',
              '/I'+str(root/'tests/psp_npc_async_stubs'),'/I'+str(root/'include'),
              str(root/'tests/test_psp_npc_async.cpp'),str(worker),'/Fe:'+str(exe)]
    else:
        args=[compiler,'-std=c++20','-pthread','-O2','-DWOWEE_TEST_NPC_ASYNC=1',
              '-I'+str(root/'tests/psp_npc_async_stubs'),'-I'+str(root/'include'),
              str(root/'tests/test_psp_npc_async.cpp'),str(worker),'-o',str(exe)]
    subprocess.run(args,cwd=temp,check=True);subprocess.run([str(exe)],check=True,timeout=20)
