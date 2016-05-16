#include <mpi.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <set>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

#define INSIDE_TAG		101
#define REQUEST_TAG		102
#define AGREE_TAG		103


using namespace std;

// DECLARATIONS

struct State {
    int rank, size;
    bool ready;
    int lamport;
    mutex mtx;
    condition_variable cv;
};


// HELPER FUNCTIONS

void randomize(int rank)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    // printf("%d: initing srand with seed %d\n", rank, tv.tv_usec + rank);
    srand(tv.tv_usec + rank);
}

long microseconds()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long) tv.tv_sec * (long) 1000000 + (long) tv.tv_usec;
}

void log(struct State *state, char *fmt, ...) {
    char *str = NULL;
    va_list args;
    va_start(args, fmt);
    vasprintf(&str, fmt, args);
    va_end(args);
    printf("%*d, %*d: %s\n", 4, state->lamport, 4, state->rank, str);
    free(str);
}


// Thread routine responsible for MPI communication

void *mpi_thread(void *arg) {
    struct State *state = (struct State *)arg;

    int buf;
    MPI::Status status;
    set<int> queue;

    bool inside = false;

    while (1) {
        MPI::COMM_WORLD.Recv(&buf, 1, MPI::INT, MPI::ANY_SOURCE, MPI::ANY_TAG, status);
        state->lamport = max(state->lamport, buf) + 1;
        switch (status.Get_tag()) {
            case INSIDE_TAG: // enter/exit
                if (!inside) {
                    for (int i = 0; i < state->size; i++) {
                        if (i != state->rank) {
                            MPI::COMM_WORLD.Send(&state->lamport, 1, MPI::INT, i, REQUEST_TAG);
                        }
                    }
                    int request_clock = state->lamport;
                    int replies_received = 0;
                    while (replies_received < state->size - 1) {
                        MPI::COMM_WORLD.Recv(&buf, 1, MPI::INT, MPI::ANY_SOURCE, MPI::ANY_TAG, status);
                        state->lamport = max(state->lamport, buf) + 1;
                        switch (status.Get_tag()) {
                            case REQUEST_TAG:
                                if (buf < request_clock || (buf == request_clock && state->rank < status.Get_source())) {
                                    // current process has higher priority
                                    queue.insert(status.Get_source());
                                } else {
                                    // other process has higher priority
                                    MPI::COMM_WORLD.Send(&state->lamport, 1, MPI::INT, status.Get_source(), AGREE_TAG);
                                }
                                break;
                            case AGREE_TAG:
                                if (buf > request_clock) {
                                    replies_received++;
                                }
                                break;
                            default:
                                log(state, "comm: Unknown message tag %d", status.Get_tag());
                        }
                    }
                    inside = true;
                    unique_lock<mutex> lck(state->mtx);
                    state->ready = true;
                    state->cv.notify_all();
                    lck.unlock();
                } else {
                    // broadcast agree to all in queue
                    for (int p : queue) {
                        MPI::COMM_WORLD.Send(&state->lamport, 1, MPI::INT, p, AGREE_TAG);
                    }
                    queue.clear();
                }
                break;
            case REQUEST_TAG:
                if (inside) {
                    queue.insert(status.Get_source());
                }
                break;
            case AGREE_TAG:
                break;
            default:
                log(state, "comm: Unknown message tag %d", status.Get_tag());
        }
    }
}


// Main program loop and state machine

void mainloop(struct State *state)
{
    int buf = 0;
    while (1) {
        int interval = rand() % 8;
        log(state, "main: Sleeping %d before", interval);
        sleep(interval);

        // notify_critical_section();
        log(state, "main: Sending enter INSIDE");
        MPI::COMM_WORLD.Send(&buf, 1, MPI::INT, state->rank, INSIDE_TAG);

        // wait_for_enter_critical_section();
        log(state, "main: Waiting for critical section...");
        unique_lock<mutex> lck(state->mtx);
        while (!state->ready) {
            state->cv.wait(lck);
        }
        lck.unlock();
        state->ready = false;

        interval = rand() % 8;
        log(state, "main: Entered critical section. Sleeping %d", interval);
        sleep(interval);

        // exit_critical_section();
        log(state, "main: Sending exit INSIDE");
        MPI::COMM_WORLD.Send(&buf, 1, MPI::INT, state->rank, INSIDE_TAG);
    }
}


// INITIALIZATION

int main(int argc, char **argv)
{
    int thread_support_provided;
    mutex mtx;
    struct State state;

    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &thread_support_provided);

    state.ready = false;
    state.rank = MPI::COMM_WORLD.Get_rank();
    state.size = MPI::COMM_WORLD.Get_size();
    state.lamport = 0;
    randomize(state.rank);
    if (state.rank == 0) {
        printf("Thread support provided: %d\n", thread_support_provided);
    }

    thread t = thread(mpi_thread, &state);

    mainloop(&state);

    t.join();
    MPI::Finalize();
}
