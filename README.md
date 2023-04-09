# **CS 140 Project 1: xv6 Rotating Staircase Deadline Scheduler**

## **Description**
For this Operating Systems project, we had to augment MIT's xv6 round-robin scheduler with
Con Kolivas' Rotating Staircase Deadline scheduler (RSDL). The RSDL uses an Active set and an Expired set,
structures separate from the process table, to help decide which process to run next.
The Active set is given a limited quantum (runtime in ticks), and upon consuming its entire quantum,
is swapped for the Expired set which is then made the new Active set. Each process is also given a limited quanta. Upon
consuming all of its quantum, the process is moved to the Expired set where it will wait for its next turn.
There are multiple *caveats* here, such as on what happens when a process consumes its entire quantum as it exits. 
Please read the Project Specs for thorough awareness of these caveats. A high-level view of RSDL is shown below:


Is defined in `rsdl.h`.
A final view of the active and expired sets is

My role for this project is mainly quality assurance with focus on optimization and debugging.
It was my job to paintstakingly test the kernel for stability and fulfillment of requirements.
Towards this, I wrote a lot of test programs, combed through a lot of output logs, and engaged
in diligent documentation and communication with my teammates to bring up suggestions for improvement.
Of course, to proceed with this, I had to have an in-depth understanding of how the xv6 kernel
works in both its original and modified forms.

A nuanced understanding of **Hardware Definition Language** (HDL) (specifically **SystemVerilog**) and digital circuits (especially the **datapath** and **control path**) is key to succeeding on this project. We modified the processor to accomodate 5 new instructions:
1. Shift left logical (sll)
2. Store byte (sb)
3. Branch on less than or equal (ble)
4. Load immediate (li)
5. Zero-from-right (zfr)

Please see my **Documentation** intro (`For Submission\Documentation.pdf`) or the MIPS Green Sheet (`MIPS Green Sheet (Berkeley).pdf`) for the MIPS32 instruction formats. You may also consult Appendix B of Harris & Harris (2013) or the Project 2 Specs (email me for access) for the complete instruction set and their expected effect.

Our technical documentation skills were also sharpened by this project, one that required *line-by-line* explanation of code. Discussed in the documentation *per added instruction* are:
* HDL edits
* Testbench for simulations. 
* Test code (in assembly and in machine code) for verifying if the extended instruction set is working.
* Demonstration that the processor can now successfully execute the instruction required.

Extreme care was also taken to maintain the baseline integrity of the single cycle MIPS processor. That is, we extended the capability of the CPU *without breaking anything* in the process. The integrity checks are the testbenches `Simulation Sources\testbench.sv` with machine code `Simulation Sources\testbench.txt` and `Simulation Sources\testbench_2.sv` with machine code `Simulation Sources\testbench_2.txt`. The instruction memory component were also tested using the `imem_testbench`.

Link to Video Documentation: https://drive.google.com/file/d/1N0l3GV2r9BShghN13NfjL3-9g5s0T68J/view?usp=sharing

## **Schematics**
Before the modifications, the SCP is only capable of handling R-type, lw, sw, and beq instructions, with the following schematic diagram (Harris & Harris, 2013):
![MIPS32_SCP_orig.png](MIPS32_SCP_orig.png)
After the modifications, the SCP is now also capable of sll, sb, ble, li, and zfr, with the following schematic diagram:
![MIPS32_SCP_final.png](MIPS32_SCP_final.png)
Full schematics can be found in `Circuit Diagrams.drawio` (draw.io needed).

## **Requirements**
It is suggested for you to only watch the video documentation or skim through the written documentation due to the project's tediousness. Design, simulation, and testbench files from the workbench (`CS21_Project2_v2\`) were also fetched for your quick reference. You may browse them in the directories `Design Sources\`, `Instruction Tests\` & `Simulation Sources\`, and `Testbenches\` respectively.

However, if you really wish to verify the results on your end, the following would be needed.
- Windows 10 or higher
- For high-performance systems, one may use Xilinx Vivado 2021.2 Suite: Download through https://www.xilinx.com/support/download/index.html/content/xilinx/en/downloadNav/vivado-design-tools/2021-2.html (will require account registration).
- For low-performance systems, one may use EDA Playground. Web app through https://edaplayground.com/ (will require account registration). Please see Project 2 Specs page 25 for full guide. 
- Minimal space and at least 4 GB memory for EDA playground. At least 70 GB of space and 8 GB of memory for Xilinx Vivado.

**Remark:** Please note that this project was accomplished through Vivado.

## **Running Simulations**
This guide will only focus on the Vivado platform. Analogous steps can be expected in EDA Playground.
1. Start Vivado. Complete the set-up and account registration process.
2. Once Vivado is started, open an existing workbench through File. Select the following directory in your clone of this repository: `CS21_Project2_v2\`. This step will load all Design and Simulation sources to your Vivado session.
3. Once the workbench has loaded, you may begin testing the added instructions. Select the appropriate testbench for the desired instruction that you will test in the Sources pane, right click it, and select "Move to top".
4. Reorder the hiearchy of files by refreshing the Sources pane.
5. Open `memfile.mem` in the Vivado Sources pane, and its contents will appear in a tab the Vivado Editor panel.
6. Copy the contents of the desired instruction test from `Instruction Tests\` to `memfile.mem` (overwrite it).
7. Finally, hit Simulate from the Menu Bar. This will produce a waveform diagram that you may examine for any errors (as we did in the documentation). You may also watch the TCL Console panel for any issues (the testbench is very verbose).

## **Sample I/O**
For testing, say, the sll instruction, you will open `Instruction Tests\sll memfile.txt` and copy its machine code contents to your `memfile.mem` (also shown below).
```
2010000c
00108200
00108200
00108200
2210000c
ac100054
```
An assembly translation of that machine code can be found in  `Instruction Tests\sll tester.asm`. Simulating the workbench with the appropriate testbench on top (`sll_testbench.sv`) will then produce the following waveform diagram (truncated to only show the last few cycles):
![sll_waveform.png](sll_waveform.png)

As expected, the diagram is clean and error-free. The TLC Console panel would also accompany this with a "Success" message. Video documentation includes a full discussion of the entire waveforms.

### **Reference**
Harris, D. M., & Harris, S. (2013). *Digital Design and Computer Architecture (2nd ed.).* Elsevier Inc. https://doi.org/10.1016/C2011-0-04377-6

---
Yenzy Urson S. Hebron

University of the Philippines Diliman

2nd Semester A.Y. 2021-2022

© Course Materials by Sir Wilson Tan and Sir Ivan Carlo Balingit

---
## **xv6 Credits**
xv6 is a re-implementation of Dennis Ritchie's and Ken Thompson's Unix
Version 6 (v6).  xv6 loosely follows the structure and style of v6,
but is implemented for a modern x86-based multiprocessor using ANSI C.

ACKNOWLEDGMENTS

xv6 is inspired by John Lions's Commentary on UNIX 6th Edition (Peer
to Peer Communications; ISBN: 1-57398-013-7; 1st edition (June 14,
2000)). See also https://pdos.csail.mit.edu/6.828/, which
provides pointers to on-line resources for v6.

xv6 borrows code from the following sources:
    JOS (asm.h, elf.h, mmu.h, bootasm.S, ide.c, console.c, and others)
    Plan 9 (entryother.S, mp.h, mp.c, lapic.c)
    FreeBSD (ioapic.c)
    NetBSD (console.c)

The following people have made contributions: Russ Cox (context switching,
locking), Cliff Frey (MP), Xiao Yu (MP), Nickolai Zeldovich, and Austin
Clements.

We are also grateful for the bug reports and patches contributed by Silas
Boyd-Wickizer, Anton Burtsev, Cody Cutler, Mike CAT, Tej Chajed, eyalz800,
Nelson Elhage, Saar Ettinger, Alice Ferrazzi, Nathaniel Filardo, Peter
Froehlich, Yakir Goaron,Shivam Handa, Bryan Henry, Jim Huang, Alexander
Kapshuk, Anders Kaseorg, kehao95, Wolfgang Keller, Eddie Kohler, Austin
Liew, Imbar Marinescu, Yandong Mao, Matan Shabtay, Hitoshi Mitake, Carmi
Merimovich, Mark Morrissey, mtasm, Joel Nider, Greg Price, Ayan Shafqat,
Eldar Sehayek, Yongming Shen, Cam Tenny, tyfkda, Rafael Ubal, Warren
Toomey, Stephen Tu, Pablo Ventura, Xi Wang, Keiichi Watanabe, Nicolas
Wolovick, wxdao, Grant Wu, Jindong Zhang, Icenowy Zheng, and Zou Chang Wei.

The code in the files that constitute xv6 is
Copyright 2006-2018 Frans Kaashoek, Robert Morris, and Russ Cox.

ERROR REPORTS

We switched our focus to xv6 on RISC-V; see the mit-pdos/xv6-riscv.git
repository on github.com.

BUILDING AND RUNNING XV6

To build xv6 on an x86 ELF machine (like Linux or FreeBSD), run
"make". On non-x86 or non-ELF machines (like OS X, even on x86), you
will need to install a cross-compiler gcc suite capable of producing
x86 ELF binaries (see https://pdos.csail.mit.edu/6.828/).
Then run "make TOOLPREFIX=i386-jos-elf-". Now install the QEMU PC
simulator and run "make qemu".