
Content is user-generated and unverified.
🚨 SERC Mini-OS
A Smart Emergency Response Center (SERC) operating system simulation built in C, designed to manage high-stakes emergency dispatch processes in real time. Features a live multi-panel terminal dashboard visualizing kernel operations including CPU scheduling, memory allocation, deadlock detection, and process management.
Built as a university project at The Copperbelt University — Department of Computer Science (CS225 Operating Systems).
________________________________________
✨ Features
•	3 CPU Scheduling Algorithms — Switch between FCFS, Round Robin (Q=3), and Priority Scheduling with Aging at runtime
•	Best-Fit Memory Allocator — Simulates 1024 MB of RAM using a linked-list with coalescing to combat fragmentation
•	Deadlock Detection & Resolution — Heuristic-based OOM Killer terminates the lowest-priority blocked process to recover the system
•	5-State Process Machine — Processes transition through NEW → READY → RUNNING → WAITING → TERMINATED
•	Live ncurses Dashboard — 5-panel TUI showing memory usage, process table, scheduling metrics, and a scrollable kernel log
•	Emergency Task Types — Handles Fire Alerts (Priority 1), Ambulance Requests (Priority 2), and Police Responses (Priority 3)
•	Persistent Audit Log — All kernel events are written to serc_kernel.log with Unix timestamps
________________________________________
🖥️ Dashboard Preview
[ v2.0 ]              SERC MINI-OS KERNEL DASHBOARD
─ STATUS ──────────────────────────────────────────────────────
MEMORY
[████████████████████████░░░░░░] 63%  650 / 1024 MB

PROCESSES               PROCESS TABLE
New:       0            PID  TASK NAME         PRI  MEM  STATE    PROGRESS
Ready:     2            1    fire at cbu        1    120  WAITING  [──────────────]
Running:   0            2    fire in riverside  1    400  READY    [──────────────]
Waiting:   3            3    patient at vml     2    250  READY    [──────────────]
Terminated: 0
─ KERNEL LOG ───────────────────────────────────────────────────
[0000] Task 'fire at cbu' (PID 1) → READY. 120 MB allocated via Best-Fit.
[0000] DEADLOCK DETECTED: memory exhausted + tasks blocked.
[0000] OOM Killer: PID 3 terminated. Deadlock resolved.
________________________________________
🛠️ Built With
Component	Technology
Language	C (C99 Standard)
GUI	ncurses (TUI)
Memory Model	Linked-list with Best-Fit + Coalescing
Scheduler	FCFS / Round Robin / Priority+Aging
Platform	Linux
________________________________________
🚀 Getting Started
Prerequisites
bash
sudo apt update
sudo apt install gcc libncurses5-dev libncursesw5-dev
Requires a terminal emulator supporting 256 colors (e.g. xterm-256color) and minimum 80×24 screen size. 100+ columns recommended.
Build & Run
bash
# Clone the repository
git clone https://github.com/yourusername/serc-mini-os.git
cd serc-mini-os

# Compile
gcc -Wall -Wextra -O2 -o serc_os_gui serc_os_gui.c -lncurses

# Run
./serc_os_gui
________________________________________
⌨️ Controls
Key	Action
1	Add new emergency task (name, type, memory)
2	Suspend a process (READY → WAITING)
3	Run FCFS scheduler
4	Run Round Robin scheduler (Q=3)
5	Run Priority scheduler with Aging
6	Trigger deadlock detection + OOM resolution
7	View full scrollable kernel log
0/Q	Shutdown and exit
________________________________________
🧠 How It Works
CPU Scheduling
•	FCFS — Non-preemptive; processes run in arrival order
•	Round Robin — Preemptive with a fixed time quantum of 3 ticks; prevents starvation
•	Priority + Aging — Fire=1 (highest), Ambulance=2, Police=3 (lowest). Every 3 scheduler cycles a waiting process's priority improves by 1 to prevent starvation
Memory Management
•	Simulates 1024 MB of physical RAM
•	Best-Fit allocation finds the smallest suitable free block to reduce fragmentation
•	Coalescing merges adjacent free blocks on process termination
Deadlock Handling
A deadlock is suspected when Count(WAITING) > 0 AND AvailableMemory < 100 MB. The OOM Killer then terminates the lowest-priority blocked process to free resources.
________________________________________
🗺️ Roadmap
•	 FCFS, Round Robin, Priority+Aging scheduling
•	 Best-Fit memory allocation with coalescing
•	 Deadlock detection and OOM resolution
•	 Live ncurses dashboard
•	 Persistent kernel audit log
•	 Windows support
•	 Inter-Process Communication (IPC) via message queues
•	 Virtual memory with paging and swap simulation
•	 Multi-threaded background scheduler
________________________________________
👥 Team — Group 62
Name	Contribution
Liam Yundayunda	CPU Scheduling and TUI command interface
Christine Chungu	File management and logging
Sindiso F Phiri	Process management
Mathews Mikondo	Creation of GUI and Memory Management
	
Copperbelt University — CS225 Operating Systems
________________________________________
📄 License
This project is open source and available under the MIT License.

