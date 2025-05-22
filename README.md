# Airport Traffic Controller Simulation

## Description

This project simulates an air traffic controller managing multiple plane processes using shared memory and UNIX signals. Planes navigate from the edges of a 2D space toward a central landing zone, while a Round Robin scheduler and collision-resolution logic ensure safe landings.

## Requirements

* POSIX-compliant OS (Linux, macOS)

## Compilation

```bash
gcc -o aeroporto aeroporto.c -lm 
```

## Usage

```bash
./aeroporto <number_of_planes>
```

* `<number_of_planes>`: Number of plane processes to spawn (max 60)

## Controls

| Command          | Action                                |
| ---------------- | ------------------------------------- |
| **Ctrl+C**       | Start/Pause simulation (flip-flop)    |
| **Ctrl+Z**       | Display status of all planes          |
| **Ctrl+\\**      | Terminate simulation                  |
| `kill -10 <pid>` | Toggle individual plane pause/resume  |
| `kill -12 <pid>` | Toggle individual plane runway switch |

## Data Structures

```c
#define MAX_AVIOES 60       // Max planes
#define VELOCIDADE 0.05     // Movement speed
#define TEMPO_ROUND_ROBIN 1 // Scheduler time quantum
#define DIST_COLISAO 0.1    // Collision threshold
#define DESTINO 0.5f        // Landing coordinates (0.5,0.5)

typedef struct {
    pid_t pid;           // Process ID
    float x, y;          // 2D position
    char origem;         // Origin side ('W' or 'E')
    int pista_pref;      // Preferred runway
    int ativo;           // Active flag (1=active, 0=terminated)
    int confirm_usr1;    // SIGUSR1 confirmation
    int confirm_usr2;    // SIGUSR2 confirmation
    int parado;          // Pause flag (1=paused, 0=running)
    pid_t ultimo_colidido; // Last collided plane PID
    int trocou_quantum;    // Guard to prevent multi-switch per quantum
} Aviao;

typedef struct {
    Aviao avioes[MAX_AVIOES];
    int n_avioes;
    int escalonamento_ativo; // Simulation active flag
    int abortados;           // Aborted planes count
    int pousados;            // Landed planes count
} EspacoAereo;
```

## Main Components

### Auxiliary Functions

* `encontra_meu_idx()`: Locate current plane index in shared memory
* `algum_ativo()`: Check for any active plane
* `dist_entre(i, j)`: Compute distance between two planes
* `dist_centro()`: Compute distance to landing center
* `pista_alternativa()`: Determine alternate runway
* `mata_aviao(idx)`: Safely abort a plane process

### Plane Lifecycle (`cria_avioes`, `movimento`)

* Random plane initialization (position, origin, runway) with `srand(getpid())`
* Continuous movement toward center; landing upon proximity threshold

### Collision Resolution (`resolve_conflito`)

1. **First encounter**: Plane farther from center switches runway (`SIGUSR2`)
2. **Repeat encounter**: Plane farther from center is paused (`SIGUSR1`)
3. **Prevent congestion**: Approaching paused planes are aborted

### Scheduler (`escalonador`)

* Implements Round Robin: resumes (`SIGCONT`) each active plane for one quantum, then pauses (`SIGSTOP`)
* Performs collision checks after each time slice

### Signal Interface (`interface_sinais`)

Registers handlers for:

* `SIGINT` (Ctrl+C): Toggle simulation on/off
* `SIGTSTP` (Ctrl+Z): Display plane statuses
* `SIGQUIT` (Ctrl+\\): Terminate simulation
* `SIGUSR1`: Global plane pause/resume
* `SIGUSR2`: Global runway switch