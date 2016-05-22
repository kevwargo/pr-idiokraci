#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <mpi.h>
#include <omp.h>
#include <ctime>
#include <vector>

/*
 * Projekt IDIOKRACJA
 *
 * Mamy N firm, ktore co jakis czas odkrywaja, ze zglosila sie do nich
 * pewna liczba idiotow. Nastepnie ci idioci moga byc operowani w klinice.
 * Mozemy rownoczesnie operowac K idiotow.
 * Nastepnie zalatwiamy papierkologie, czyli firmy ubiegaja sie do jednego
 * z L okienek.
 *
 * Mozemy wyroznic nastepujace stany:
 * Stan 0- rozpoczecie programu, przygotowanie srodowiska
 * Stan 1- czekanie na idiotow
 * Stan 2- dopoki nie wyczerpiemy wszystkich idiotow
 *      Stan 2a- ubieganie sie o miejsce w klinice
 *      Stan 2b- przebywanie w klinice
 *      Stan 2c- opuszczanie kliniki
 * Stan 3- ubieganie sie o okno
 * Stan 4- papierkologia
 *
 * Wyrozniamy w stanach 1, 2b oraz 4 dwa watki:
 * Do komunikacji- odpowiada za komunikacje procesow ze soba
 * Do sterowania- informuje, kiedy skonczyc szukanie
 *
*/

// Message Tags
#define INSIDE           0
#define KLINIKA_REQUEST  1
#define KLINIKA_AGREE    2
#define OKNO_REQUEST     3
#define OKNO_AGREE       4

typedef struct {
    int pid;
    int tim;
    int val;
} tmessage;

std::vector<tmessage> klinikainside;
std::vector<tmessage> klinikawaiting;
std::vector<tmessage> okienkawaiting;



// Program constants
const int max_idiots   = 20; // maksymalna liczba idiotow
const int max_wait_i   = 2; // maksymalny czas oczekiwania na idiotow
const int max_wait_k   = 2; // maksymalny czas oczekiwania na klinike
const int max_wait_o   = 2; // maksymalny czas oczekiwania na okienko

// Program parameters
int id,        // Id firmy / procesu
    N,         // Liczba firm / procesow
    K,         // Liczba miejsc w klinice
    L;         // Liczba okienek w urzedzie

// Program variables
int idiots;    // Liczba idiotow
int lamport;   // Zegar Lamporta, poczatkowa wartosc to 0
int tmp_idiots;// Poprzednia liczba idiotow, jest trzymana na potrzeby wyslania wiadomosci o zwolnieniu kliniki

int miejscaZajete() {
    int res = 0;
    for (int i = 0; i < klinikainside.size(); i++) {
        res += klinikainside.at(i).val;
    }
    return res;
}

// STAN 1-----------------------------------------------------------------------

// Kod watku sterujacego w stanie 1
void state1Control() {
    // Firma czeka az pojawia sie nowi idioci
    sleep(rand() % max_wait_i);

    tmessage message;
    message.pid = id;     // ID procesu, wysylamy sami do siebie
    message.tim = -1;     // Tu normalnie zegar Lamporta, lecz wiadomosci INSIDE
                          // korzystaja z zegaru Lamporta
    message.val = 0;      // Nie mamy konkretnej wartosci do podeslania
    MPI_Send(&message, 3, MPI_INT, id, INSIDE, MPI_COMM_WORLD);
}

// Kod watku komunikacyjnego w stanie 1
void state1Communication() {
    /*
     * Firma w trakcie czekania na idiotow musi odpowiadac na prosby
     * innych firm
     */
    MPI_Status status;

    tmessage recvmessage;

    do {
        tmessage message;
        MPI_Recv(&recvmessage, 3, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        switch (status.MPI_TAG) {
        case KLINIKA_REQUEST:  // nie ubiegamy sie o sekcje, wiec od razu wysylamy AGREE
            printf("%d %d : Firma <%d> oczekuje na idiotow, otrzymala wiadomosc KLINIKA_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            klinikainside.push_back(recvmessage);
            message.pid = id;
            message.tim = lamport;
            message.val = 0;
            MPI_Send(&message, 3, MPI_INT, status.MPI_SOURCE, KLINIKA_AGREE, MPI_COMM_WORLD);
            lamport++;
            printf("%d %d : Firma <%d> oczekuje na idiotow, wysyla wiadomosc KLINIKA_AGREE do %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            break;
        case OKNO_REQUEST:     // nie ubiegamy sie o sekcje, wiec od razu wysylamy AGREE
            printf("%d %d : Firma <%d> oczekuje na idiotow, otrzymala wiadomosc OKNO_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            message.pid = id;
            message.tim = lamport;
            message.val = 0;
            MPI_Send(&message, 3, MPI_INT, status.MPI_SOURCE, OKNO_AGREE, MPI_COMM_WORLD);
            lamport++;
            printf("%d %d : Firma <%d> oczekuje na idiotow, wysyla wiadomosc OKNO_AGREE do %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            break;
        default:
            if (status.MPI_TAG != INSIDE) {
                lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
                lamport++;
            }
        }
    } while (status.MPI_TAG != INSIDE);
    idiots = rand() % max_idiots; // Tutaj przychodza idioci do firmy
    printf("%d %d : Firma <%d> otrzymala %d idiotow\n", lamport, id, id, idiots);
}

// STAN 2a----------------------------------------------------------------------

void state2aCommunication() {  // Ten stan wymaga tylko komunikacji

    /*
     * Ta funkcja odpowiada za stan, w ktorym firma ubiega sie o dostep do kliniki.
     * Zatem, gdy odbieramy wiadomosc:
     * -KLINIKA_REQUEST, to porownujemy priorytet i albo uznajemy,ze mamy wiekszy
     *   i automatycznie uznajemy swoje prawo do sekcji wzgledem tamtego procesu,
     *   i inkrementujemy licznik zgod
     *   LUB widzimy, ze mamy mniejszy priorytet i ustepujemy temu procesowi
     * -KLINIKA_AGREE, to inkrementujemy licznik zgod
     * -OKNO_REQUEST, to dajemy zgode
     *
    */

    MPI_Status status;

    tmessage request;
    request.pid = id;      // Nasze id, potrzebne do priorytetu
    request.tim = lamport; // Nasz zegar
    request.val = idiots;  // Ilu idiotow chcemy oddac do badan

    for (int i = 0; i < N; i++) {
        if (i != id) {
            MPI_Send(&request, 3, MPI_INT, i, KLINIKA_REQUEST, MPI_COMM_WORLD);
        }
    }

    printf("%d %d : Firma <%d> wyslala broadcast KLINIKA_REQUEST\n", lamport, id, id);

    lamport++;    //Inkrementacja zegara Lamporta po wyslaniu broadcastu

    tmessage recvmessage;

    int agreements = 0;

    bool * agree = new bool[N];

    do {
        tmessage message;
        MPI_Recv(&recvmessage, 3, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        switch (status.MPI_TAG) {
        case KLINIKA_REQUEST:  // ubiegamy sie o sekcje, AGREE zalezy od priorytetu
            printf("%d %d : Firma <%d> oczekuje na klinike, otrzymala wiadomosc KLINIKA_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            // Nalezy podjac decyzje, kto ma pierwszenstwo do kliniki
            if ((lamport < recvmessage.tim) || ((lamport == recvmessage.tim) && (id < recvmessage.pid))) {
                // Mam pierwszenstwo do kliniki
                klinikawaiting.push_back(recvmessage);
                printf("%d %d : Firma <%d> oczekuje na klinike, otrzymuje pierwszenstwo przed %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
                if (!agree[recvmessage.pid]) {
                    agreements++;
                    agree[recvmessage.pid] = true;
                }
            }
            else {
                klinikainside.push_back(recvmessage);
                printf("%d %d : Firma <%d> oczekuje na klinike, nie ma pierwszenstwa przed %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            }
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            break;
        case OKNO_REQUEST:     // nie ubiegamy sie o sekcje, wiec od razu wysylamy AGREE
            printf("%d %d : Firma <%d> oczekuje na klinike, otrzymala wiadomosc OKNO_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            message.pid = id;
            message.tim = lamport;
            message.val = 0;
            MPI_Send(&message, 3, MPI_INT, status.MPI_SOURCE, OKNO_AGREE, MPI_COMM_WORLD);
            lamport++;
            printf("%d %d : Firma <%d> oczekuje na klinike, wysyla wiadomosc OKNO_AGREE do %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            break;
        case KLINIKA_AGREE:   // gdy otrzymujemy zgode, to inkrementujemy licznik zgod
            printf("%d %d : Firma <%d> oczekuje na klinike, otrzymala wiadomosc KLINIKA_AGREE %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            if (recvmessage.val > 0) { //Jezeli ktos zwalnia miejsce w klinice i wysyla nam zgode, to musimy go usunac z naszej listy obecnych w klinice
                int i = 0;
                while (i < klinikainside.size() && klinikainside.at(i).pid != recvmessage.pid) i++;
                if (i < klinikainside.size()) klinikainside.erase(klinikainside.begin()+i);
            }
            if (!agree[recvmessage.pid]) {
                agreements++;
                agree[recvmessage.pid] = true;
            }
            break;
        default:
            if (status.MPI_TAG != INSIDE) {
                lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
                lamport++;
            }
        }
    } while (agreements < N - 1);

    printf("%d %d : Firma <%d> otrzymala dostep do kliniki z %d idiotami\n", lamport, id, id, K - miejscaZajete() < idiots ? K - miejscaZajete() : idiots);

    tmp_idiots = idiots;
    klinikainside.push_back(request);
    idiots = idiots - K - miejscaZajete() > 0 ? idiots - K - miejscaZajete() : 0;


    delete [] agree;
}

// STAN 2b----------------------------------------------------------------------

// Kod watku sterujacego w stanie 2b
void state2bControl() {
    // Firma czeka az pojawia sie nowi idioci
    sleep(rand() % max_wait_k);

    tmessage message;
    message.pid = id;     // ID procesu, wysylamy sami do siebie
    message.tim = -1;     // Tu normalnie zegar Lamporta, lecz wiadomosci INSIDE
                          // korzystaja z zegaru Lamporta
    message.val = 0;      // Nie mamy konkretnej wartosci do podeslania
    MPI_Send(&message, 3, MPI_INT, id, INSIDE, MPI_COMM_WORLD);
}

void state2bCommunication() {
    MPI_Status status;

    tmessage recvmessage;

    do {
        tmessage message;
        MPI_Recv(&recvmessage, 3, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        switch (status.MPI_TAG) {
        case KLINIKA_REQUEST:  // Jestesmy w klinice, zatem najpierw sprawdzamy, czy wg nas jest miejsce w klinice i wtedy wysylamy wiadomosc
            printf("%d %d : Firma <%d> jest w klinice, otrzymala wiadomosc KLINIKA_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            if (miejscaZajete() < K) {
                klinikainside.push_back(recvmessage);
                message.pid = id;
                message.tim = lamport;
                message.val = 0;
                MPI_Send(&message, 3, MPI_INT, status.MPI_SOURCE, KLINIKA_AGREE, MPI_COMM_WORLD);
                lamport++;
                printf("%d %d : Firma <%d> jest w klinice, wysyla wiadomosc KLINIKA_AGREE do %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            }
            else {
                klinikawaiting.push_back(recvmessage);
                printf("%d %d : Firma <%d> jest w klinice, w ktorej nie ma miejsca, wiec nie wysyla AGREE do %d %d\n", lamport, id, id, message.tim, message.pid);
            }
            break;
        case OKNO_REQUEST:     // nie ubiegamy sie o sekcje, wiec od razu wysylamy AGREE
            printf("%d %d : Firma <%d> jest w klinice, otrzymala wiadomosc OKNO_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            message.pid = id;
            message.tim = lamport;
            message.val = 0;
            MPI_Send(&message, 3, MPI_INT, status.MPI_SOURCE, OKNO_AGREE, MPI_COMM_WORLD);
            lamport++;
            printf("%d %d : Firma <%d> jest w klinice, wysyla wiadomosc OKNO_AGREE do %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            break;
        case KLINIKA_AGREE:   // gdy otrzymujemy informacje o opuszczeniu przez jedna z firm
            printf("%d %d : Firma <%d> jest w klinice, otrzymala wiadomosc KLINIKA_AGREE %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            if (recvmessage.val > 0) { //Jezeli ktos zwalnia miejsce w klinice i wysyla nam zgode, to musimy go usunac z naszej listy obecnych w klinice
                int i = 0;
                while (i < klinikainside.size() && klinikainside.at(i).pid != recvmessage.pid) i++;
                if (i < klinikainside.size()) klinikainside.erase(klinikainside.begin()+i);
                if (miejscaZajete() < K) {
                    for (int j = 0; j < klinikawaiting.size(); j++) {
                        tmessage placefree;
                        placefree.pid = id;
                        placefree.tim = lamport;
                        placefree.val = 0;
                        MPI_Send(&placefree, 3, MPI_INT, status.MPI_SOURCE, KLINIKA_AGREE, MPI_COMM_WORLD);
                    }
                    klinikawaiting.clear();
                }
            }
            break;
        default:
            if (status.MPI_TAG != INSIDE) {
                lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
                lamport++;
            }
        }
    } while (status.MPI_TAG != INSIDE);
}

// STAN 2c----------------------------------------------------------------------

void state2cCommunication() {
    tmessage leave;
    leave.pid = id;          // Nasze id, potrzebne do priorytetu
    leave.tim = lamport;     // Nasz zegar
    leave.val = tmp_idiots;  // Wartosc jest konieczna, poniewaz gdy val == 0 to procesy nie usuwaja procesu z listy firm wewnatrz kliniki

    for (int i = 0; i < klinikawaiting.size(); i++) {
        MPI_Send(&leave, 3, MPI_INT, klinikawaiting.at(i).pid, KLINIKA_AGREE, MPI_COMM_WORLD);
        printf("%d %d : Firma <%d> opuszcza klinike, wysyla zgode do skolejkowanego %d %d\n", lamport, id, id, klinikawaiting.at(i).tim, klinikawaiting.at(i).pid);
    }
    lamport++;
    klinikawaiting.clear();
    int fieldtoremove;
    for (int i = 0; i < klinikainside.size(); i++) {
        if (klinikainside.at(i).pid != id) {
            MPI_Send(&leave, 3, MPI_INT, klinikainside.at(i).pid, KLINIKA_AGREE, MPI_COMM_WORLD);
            printf("%d %d : Firma <%d> opuszcza klinike, wysyla zgode do obecnego w klinice %d %d\n", lamport, id, id, klinikainside.at(i).tim, klinikainside.at(i).pid);
        } else {
            fieldtoremove = i;
        }
    }
    lamport++;
    klinikainside.erase(klinikainside.begin() + fieldtoremove);
    printf("%d %d : Firma <%d> rozeslala zgody do skolejkowanych firm\n", lamport, id, id);
}

// STAN 3-----------------------------------------------------------------------

void state3Communication() {

    MPI_Status status;

    tmessage request;
    request.pid = id;      // Nasze id, potrzebne do priorytetu
    request.tim = lamport; // Nasz zegar
    request.val = 0;       // niezaleznie od liczby idiotow podchodzimy do jednego okienka

    for (int i = 0; i < N; i++) {
        if (i != id) {
            MPI_Send(&request, 3, MPI_INT, i, OKNO_REQUEST, MPI_COMM_WORLD);
        }
    }
    int lamporttimeonsend = lamport; // Musimy zapamietac zegar Lamporta przy wysylaniu, aby nie uznac przedawnionej zgody
                                     // z poprzedniego ubiegania sie o sekcje

    printf("%d %d : Firma <%d> wyslala broadcast OKNO_REQUEST\n", lamport, id, id);

    lamport++;    //Inkrementacja zegara Lamporta po wyslaniu broadcastu

    tmessage recvmessage;

    int agreements = 0;

    bool * agree = new bool[N];

    for (int i = 0; i < N; i++) agree[i] = false;

    do {
        tmessage message;
        MPI_Recv(&recvmessage, 3, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        switch (status.MPI_TAG) {
        case KLINIKA_REQUEST:  // nie ubiegamy sie o sekcje, wiec od razu wysylamy AGREE
            printf("%d %d : Firma <%d> oczekuje na okno, otrzymala wiadomosc KLINIKA_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            message.pid = id;
            message.tim = lamport;
            message.val = 0;
            MPI_Send(&message, 3, MPI_INT, status.MPI_SOURCE, KLINIKA_AGREE, MPI_COMM_WORLD);
            lamport++;
            printf("%d %d : Firma <%d> oczekuje na okienko, wysyla wiadomosc KLINIKA_AGREE do %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            break;
        case OKNO_REQUEST:   // ubiegamy sie o sekcje, AGREE zalezy od priorytetu
            printf("%d %d : Firma <%d> oczekuje na okno, otrzymala wiadomosc OKNO_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            if ((lamport < recvmessage.tim) || ((lamport == recvmessage.tim) && (id < recvmessage.pid))) {
                // Mam pierwszenstwo do okna
                okienkawaiting.push_back(recvmessage);
                printf("%d %d : Firma <%d> oczekuje na okienko, otrzymuje pierwszenstwo przed %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
                if (!agree[recvmessage.pid]) {
                    agreements++;
                    agree[recvmessage.pid] = true;
                }
            }
            else {
                printf("%d %d : Firma <%d> oczekuje na okienko, nie ma pierwszenstwa przed %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            }
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            break;
        case OKNO_AGREE:   // gdy otrzymujemy zgode, to inkrementujemy licznik zgod
            printf("%d %d : Firma <%d> oczekuje na okienko, otrzymala wiadomosc OKNO_AGREE %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            if (recvmessage.tim > lamporttimeonsend)
                if (!agree[recvmessage.pid]) {
                    agreements++;
                    agree[recvmessage.pid] = true;
                }
            break;
        default:
            if (status.MPI_TAG != INSIDE) {
                lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
                lamport++;
            }
        }
    } while (agreements < N - L);

    printf("%d %d : Firma <%d> otrzymala dostep do okienka\n", lamport, id, id);

    delete [] agree;
}

// STAN 4-----------------------------------------------------------------------

// Kod watku sterujacego w stanie 4
void state4Control() {
    // Firma realizuje papierkologie
    sleep(rand() % max_wait_o);

    tmessage message;
    message.pid = id;     // ID procesu, wysylamy sami do siebie
    message.tim = -1;     // Tu normalnie zegar Lamporta, lecz wiadomosci INSIDE
                          // korzystaja z zegaru Lamporta
    message.val = 0;      // Nie mamy konkretnej wartosci do podeslania
    MPI_Send(&message, 3, MPI_INT, id, INSIDE, MPI_COMM_WORLD);
}

void state4Communication() {
    MPI_Status status;

    tmessage recvmessage;

    do {
        tmessage message;
        MPI_Recv(&recvmessage, 3, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        switch (status.MPI_TAG) {
        case KLINIKA_REQUEST:
            printf("%d %d : Firma <%d> jest przy oknie, otrzymala wiadomosc KLINIKA_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            message.pid = id;
            message.tim = lamport;
            message.val = 0;
            MPI_Send(&message, 3, MPI_INT, status.MPI_SOURCE, KLINIKA_AGREE, MPI_COMM_WORLD);
            lamport++;
            printf("%d %d : Firma <%d> jest przy oknie, wysyla wiadomosc KLINIKA_AGREE do %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            break;
        case OKNO_REQUEST:
            printf("%d %d : Firma <%d> jest przy oknie, otrzymala wiadomosc OKNO_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            // jestem przy oknie
            okienkawaiting.push_back(recvmessage);
            printf("%d %d : Firma <%d> jest przy oknie, kolejkuje zadanie %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            break;
        default:
            if (status.MPI_TAG != INSIDE) {
                lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
                lamport++;
            }
        }
    } while (status.MPI_TAG != INSIDE);
    printf("%d %d : Firma <%d> skonczyla papierkologie\n", lamport, id, id);
}

// STAN 5-----------------------------------------------------------------------

void state5Communication() {
    tmessage leave;
    leave.pid = id;      // Nasze id, potrzebne do priorytetu
    leave.tim = lamport; // Nasz zegar
    leave.val = 0;       // Nieistotna wartosc

    for (int i = 0; i < okienkawaiting.size(); i++) {
        MPI_Send(&leave, 3, MPI_INT, okienkawaiting.at(i).pid, OKNO_AGREE, MPI_COMM_WORLD);
        printf("%d %d : Firma <%d> opuszcza okienko, wysyla zgode do skolejkowanego %d %d\n", lamport, id, id, okienkawaiting.at(i).tim, okienkawaiting.at(i).pid);
    }
    lamport++;
    okienkawaiting.clear();
    printf("%d %d : Firma <%d> rozeslala zgody do skolejkowanych firm\n", lamport, id, id);
}

// Odczekanie na wyslanie dodatkowych wiadomosci--------------------------------

void waitControll() {
    // Funkcja tymczasowa, na potrzeby debugowania
    sleep(10);

    tmessage message;
    message.pid = id;     // ID procesu, wysylamy sami do siebie
    message.tim = -1;     // Tu normalnie zegar Lamporta, lecz wiadomosci INSIDE
                          // korzystaja z zegaru Lamporta
    message.val = 0;      // Nie mamy konkretnej wartosci do podeslania
    MPI_Send(&message, 3, MPI_INT, id, INSIDE, MPI_COMM_WORLD);
}

void waitCommunication() {
    /*
     * To tymczasowa funkcja na potrzeby debugowania, czeka by
     * odpowiedziec reszcie firm, ktore jeszcze pracuja
     */
    MPI_Status status;

    tmessage recvmessage;

    do {
        tmessage message;
        MPI_Recv(&recvmessage, 3, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        switch (status.MPI_TAG) {
        case KLINIKA_REQUEST:  // nie ubiegamy sie o sekcje, wiec od razu wysylamy AGREE
            printf("%d %d : Firma <%d> skonczyla prace, otrzymala wiadomosc KLINIKA_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            klinikainside.push_back(recvmessage);
            message.pid = id;
            message.tim = lamport;
            message.val = 0;
            MPI_Send(&message, 3, MPI_INT, status.MPI_SOURCE, KLINIKA_AGREE, MPI_COMM_WORLD);
            lamport++;
            printf("%d %d : Firma <%d> skonczyla prace, wysyla wiadomosc KLINIKA_AGREE do %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            break;
        case OKNO_REQUEST:     // nie ubiegamy sie o sekcje, wiec od razu wysylamy AGREE
            printf("%d %d : Firma <%d> skonczyla prace, otrzymala wiadomosc OKNO_REQUEST %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
            lamport++;
            message.pid = id;
            message.tim = lamport;
            message.val = 0;
            MPI_Send(&message, 3, MPI_INT, status.MPI_SOURCE, OKNO_AGREE, MPI_COMM_WORLD);
            lamport++;
            printf("%d %d : Firma <%d> skonczyla prace, wysyla wiadomosc OKNO_AGREE do %d %d\n", lamport, id, id, recvmessage.tim, recvmessage.pid);
            break;
        default:
            if (status.MPI_TAG != INSIDE) {
                lamport = lamport > recvmessage.tim ? lamport : recvmessage.tim;
                lamport++;
            }
        }
    } while (status.MPI_TAG != INSIDE);
    idiots = rand() % max_idiots; // Tutaj przychodza idioci do firmy
    printf("%d %d : Firma <%d> otrzymala %d idiotow\n", lamport, id, id, idiots);
}

// MAIN-------------------------------------------------------------------------

int main(int argc, char * argv[]) {

    // Inicjalizacja srodowiska MPI
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    MPI_Comm_size(MPI_COMM_WORLD, &N);
    MPI_Comm_rank(MPI_COMM_WORLD, &id);

    srand(time(NULL)+id);

    if (argc < 3) {
        printf("\nNie uruchomiono prawidlowo programu.\n"
               "Prawidlowe uruchomienie to:\nmpirun -np <N> %s <K> <L>\n"
               "Gdzie N- liczba firm, "
               "K- miejsca w klinice, L- liczba okien\n", argv[0]);
        MPI_Finalize();
        return -1;
    }

    K = atoi(argv[1]); // Zadeklarowanie miejsc w klinice
    L = atoi(argv[2]); // Zadeklarowanie okienek w urzedzie

    while (1) {

        // STAN 1 oczekiwanie na idiotow

        #pragma omp parallel sections num_threads(2)
        {
            #pragma omp section
            {
                state1Control();
            }
            #pragma omp section
            {
                state1Communication();
            }
        }
    /*
        // STAN 2 klinika

        do {
            // STAN 2a ubieganie sie o kklinika

            state2aCommunication();

            // STAN 2b przebywanie w klinice

            #pragma omp parallel sections
            {
                #pragma omp section
                {
                    state2bControl();
                }
                #pragma omp section
                {
                    state2bCommunication();
                }
            }

            // STAN 2c zwolnienie miejsc w klinice

            state2cCommunication();

        } while (idiots > 0);

    */
        // STAN 3 ubieganie sie o okno

        state3Communication();

        // STAN 4 przebywanie przy okienku

        #pragma omp parallel sections num_threads(2)
        {
            #pragma omp section
            {
                state4Control();
            }
            #pragma omp section
            {
                state4Communication();
            }
        }

        // STAN 5 zwolnienie okienek

        state5Communication();

    }
    // ODCZEKANIE NA INNE PROCESY

    /*#pragma omp parallel sections num_threads(2)
    {
        #pragma omp section
        {
            waitControll();
        }
        #pragma omp section
        {
            waitCommunication();
        }
    }

    printf("KONIEC PRACY PROCESU!!!\n");*/

    MPI_Finalize();
    return 0;
}
