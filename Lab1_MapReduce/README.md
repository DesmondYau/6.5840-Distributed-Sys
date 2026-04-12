<img width="1536" height="1024" alt="Copilot_20260412_215806" src="https://github.com/user-attachments/assets/baa6fbc2-a7f7-433b-99f2-8a312fdc4f47" />
# MIT 6.824 Lab 1 – MapReduce (C++ Implementation)

## Overview

This project is a C++ implementation of **MIT 6.824 Lab 1: MapReduce**, demonstrating distributed task coordination using gRPC. The goal is to design a fault-tolerant Master–Worker system that executes map and reduce tasks in parallel, handles worker crashes gracefully, and produces deterministic output identical to the sequential baseline.

---

## Architecture Summary

### Master Node

The **Master** acts as the central coordinator. It manages two vectors:

* `std::vector<Task::ptr> m_mapTasks` – stores all map tasks.
* `std::vector<Task::ptr> m_reduceTasks` – stores all reduce tasks.

Each task tracks its ID, filename, type, state (`UNASSIGNED`, `ASSIGNED`, `COMPLETED`), and deadline.

##### Key Responsibilities

* **Task Assignment:** When a worker requests a task, the Master assigns an unassigned map or reduce task via gRPC.
* **Progress Tracking:** Workers report completion using RPC calls, updating task states and remaining counters.
* **Fault Recovery:** A background thread resets timed-out tasks to `UNASSIGNED` if deadlines expire.
* **Graceful Shutdown:** Another thread monitors completion and shuts down the gRPC server when all tasks finish.

##### Concurrency Control

All task operations are protected by `std::mutex` to ensure thread-safe updates.

---

### Worker Nodes

Workers communicate with the Master through **gRPC RPCs** defined in `MasterServiceImpl`:

* `GetTaskForWorker()` – Request a new task.
* `ReportMapComplete()` / `ReportReduceComplete()` – Notify completion.
* `GetMapCount()` / `GetReduceCount()` – Retrieve configuration.

Each worker repeatedly:

1. Sends a gRPC request for a task.
2. Executes the assigned map or reduce logic.
3. Reports completion back to the Master.

---

### Threads in Master

Two concurrent threads maintain system health:

* **Task Reset Thread:** Checks deadlines every 2 seconds and resets stalled tasks.
* **Monitor Thread:** Checks every 10 seconds if all tasks are complete and shuts down the server.

---

## Execution Flow

1. **Initialization:** Master creates vectors of map and reduce tasks.
2. **Task Assignment:** Workers request tasks via gRPC; Master assigns and timestamps them.
3. **Execution & Reporting:** Workers process data and report completion.
4. **Fault Recovery:** Timed-out tasks are re-queued automatically.
5. **Completion:** When all tasks are done, Master shuts down gracefully.

---

## How to Run

```bash
# Build using CMake
mkdir -p build && cd build
cmake ..
make

### Run Tests
There is a test-mr.sh script in the tests folder that automates running a suite of MapReduce tests for correctness and parallelism.
``bash
./tests/test-mr.sh

---

## Technical Highlight
- **Language:** C++23
- **RPC Framework:** gRPC
- **Concurrency:** std::thread, std::mutex
- **Timeout Handling:** std::chrono
- **Design Pattern:** Master–Worker coordination


