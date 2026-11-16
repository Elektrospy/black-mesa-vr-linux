Stand-in test rig for the native Linux shim (no GPU, no game, no 32-bit libc needed).
Builds the shim from linux/bmvr_preload.c as a 64-bit library and runs it against a fake libtogl,
a fake GL (FBO state, lazy FBO binding) and a mock helper speaking linux/bmvr_xfer.h.

  S=linux; T=tools/linux/standin-test; W=$(mktemp -d); mkdir $W/work
  gcc -shared -fPIC -o $W/libfaketogl.so $T/faketogl.c
  gcc -std=gnu11 -fPIC -shared -fno-strict-aliasing -fno-stack-protector -I$S -o $W/bmvr_preload64.so $S/bmvr_preload.c -ldl
  gcc -std=gnu11 -rdynamic -o $W/work/bms_linux $T/fakegl_driver.c -L$W -lfaketogl -Wl,-rpath,'$ORIGIN/..' -ldl
  cp $T/mock_helper.py $T/run.sh $T/run_cfg.sh $W/ && $W/run.sh normal      # modes: normal lazy nosurf nocontext nohelper
  (env: BMVR_TARGET=size:128x128 | name:msaa | window ; DIE_AFTER=2 for the mock helper)
This tests the shim's logic only. It says nothing about what the real ToGL / driver does.
