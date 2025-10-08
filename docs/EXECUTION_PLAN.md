Read these documents:

docs/BM1398_PROTOCOL.md

docs/PATTERN_TEST.md

Here is the bmminer binary, with Ghidra, and IDA Pro decompilations and assembly files.

[EXTERNAL]/Bitmain_Peek/S19_Pro/Antminer-S19-Pro-merge-release-20221226124238/Antminer S19 Pro/zynq7007_NBP1901/update/minerfs.no_header.image_extract/_ghidra/bins/bmminer-2f464d0989b763718a6fbbdee35424ae

[EXTERNAL]/Bitmain_Peek/S19_Pro/Antminer-S19-Pro-merge-release-20221226124238/Antminer S19 Pro/zynq7007_NBP1901/update/minerfs.no_header.image_extract/usr/bin

Here is also single_board_test, the Bitmain test fixture tool used to test a hashboard, and perform pattern testing
When single_board_test is ran, it assumes the connected hashboard is already receiving power, however in our case we need to power on the PSU as bmminer does, and then power off once done testing.

[EXTERNAL]/Bitmain_Test_Fixtures/S19_Pro

We also have the binary_ninja_mcp MCP server available with single_board_test and bmminer binaries loaded and ready for analysis.

Here are also logs from bmminer that may be useful to understand how it works:

docs/bmminer_s19pro_68_7C_2E_2F_A4_D9.log

And bmminer debug logs:

docs/bmminer_s19pro_68_7C_2E_2F_A4_D9_debug.log

We are reimplementing bmminer and single_board_test, here is our current source code:
hashsource_x19/include/bm1398_asic.h
hashsource_x19/src/bm1398_asic.c
hashsource_x19/src/chain_test.c
hashsource_x19/src/pattern_test.c
hashsource_x19/src/work_test.c

The goal is to get at least one hashboard hashing, chain 0, use the docs we have and the Bitmain binaries to achieve that goal.

I need you to analyze our correct source code, and figure out what details we are missing, compared to the Bitmain binaries.
You also need to verify every detail, FPGA registers, initialization code, work generation, pattern testing code compared to BM1398_PROTOCOL doc, PATTERN_TEST doc, and bmminer, single_board_test binaries.

These programs have been verified to be working, use them as reference to understand fan and PSU controls.
Verify in our pattern test program and work submission that the PSU is enabled and powered on properly, otherwise the hashboard and ASICs won't respond.

hashsource_x19/src/fan_test.c
hashsource_x19/src/psu_test.c

Don't create new documents with your analysis, fix our code directly.
When writing code, the code should be concise, pragmatic, maintainable, idiomatic, modern, type-safe, secure and performant.

Once you make your corrections, rebuild the code, and deploy to the HashSource machine and test it.
docs/TEST_MACHINES.md

We are using buildroot to build the firmware, use the ARM compiler to compile the code, check the Makefile to understand how to rebuild the code:
Makefile
hashsource_x19/Makefile
