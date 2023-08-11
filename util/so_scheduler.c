#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include "so_scheduler.h"

#define NUMAR_MAXIM_THREADURI 1000

// va trebui sa stiu despre fiecare thread starea in care se afla la un moment dat
// pentru a sti ce sa fac cu el

enum stare_thread
{
    nou_creat,
    gata,
    ruleaza,
    asteapta,
    terminat
};

typedef enum stare_thread stare_thread;

struct thread
{
    stare_thread stare;
    int prioritate;    // un numar intre 0 si 5
    int cuanta_ramasa; // cate instructiuni mai are de executat thread-ul
    int dispozitiv_io;  // ce dispozitiv I/O foloseste thread-ul
    tid_t id_thread;     // id-ul thread-ului
    so_handler *handler; // handler-ul specific thread-ului (ce face thread-ul); un pointer la o functie
    sem_t semafor;       // primitiva de sincronizare a thread-ului (arata cand pot rula un thread/nu)
};

typedef struct thread thread;

struct planificator
{
    int numar_io;                    // numarul maxim de evenimente i/o admis este 256
    int cuanta_de_timp;              // cate instructiuni suporta un thread
    int numar_de_threaduri;          // cate thread-uri sunt planificate in total
    int numar_de_threaduri_in_coada; // cate thread-uri contine coada la un moment dat
    thread *thread_curent;           // ce thread ruleaza pe procesor la un moment dat
    thread **threaduri;              // vector de thread*; adresele tuturor thread-urilor create
    thread **coada;                  // coada de prioritati ce va contine adrese catre thread-urile create; este necesar algoritmului Round Robin
};

typedef struct planificator planificator;

planificator *planificator_de_threaduri;
int initializat = 0;

// functia insereaza un nou thead in coada
void adauga_in_coada(thread *thread_adaugat)
{
    if (thread_adaugat == NULL)
        return;

    if (!(planificator_de_threaduri->numar_de_threaduri_in_coada))
    {
        (planificator_de_threaduri->coada)[0] = thread_adaugat;
        planificator_de_threaduri->numar_de_threaduri_in_coada++;
    }
    else
    {
        (planificator_de_threaduri->coada)[planificator_de_threaduri->numar_de_threaduri_in_coada] = thread_adaugat;
        planificator_de_threaduri->numar_de_threaduri_in_coada++;

        // se mentine coada ordonata descrescator dupa prioritate
        int i,j;
        for(i = 0; i < planificator_de_threaduri->numar_de_threaduri_in_coada - 1; i++)
            for(j = i + 1; j < planificator_de_threaduri->numar_de_threaduri_in_coada; j++)
            {
                if ((planificator_de_threaduri->coada)[i]->prioritate < (planificator_de_threaduri->coada)[j]->prioritate)
                {
                    thread *aux = (planificator_de_threaduri->coada)[i];
                    (planificator_de_threaduri->coada)[i] = (planificator_de_threaduri->coada)[j];
                    (planificator_de_threaduri->coada)[j] = aux;
                }
            }
    }
}

// functia intoarce primul element din coada (care exista), mai exact urmatorul
// thread ce urmeaza sa fie rulat pe procesor
thread *primul_din_coada()
{
    if (planificator_de_threaduri->numar_de_threaduri_in_coada)
        return (planificator_de_threaduri->coada)[0];

    return NULL;
}

// functia elimina primul element din coada in urma punerii thread-ului cu
// cea mai mare prioritate din aceasta pe procesor
void elimina_din_coada()
{
    int i;
    for (i = 0; i < planificator_de_threaduri->numar_de_threaduri_in_coada - 1; i++)
        (planificator_de_threaduri->coada)[i] = (planificator_de_threaduri->coada)[i + 1];

    (planificator_de_threaduri->coada)[planificator_de_threaduri->numar_de_threaduri_in_coada - 1] = NULL;
    planificator_de_threaduri->numar_de_threaduri_in_coada--;
}

// functia se ocupa cu initializarea planificatorului de thread-uri
// aceasta aloca memorie pentru campurile structurii si le initializeaza
int so_init(unsigned int time_quantum, unsigned int io)
{
    // se verifica corectitudinea parametrilor primiti:
    // in mod normal thread-urile au cel putin 1 instructiune de executat
    // si nu pot exista mai multe evenimente i/o decat numarul maxim admis
    // cazuri in care functia returneaza un numar negativ

    if (time_quantum < 1)
        return -1;

    if (io > SO_MAX_NUM_EVENTS)
        return -1;

    // planificatorul poate fi alocat o singura data

    if (initializat)
        return -1;

    // se aloca memorie pentru planificator
    planificator_de_threaduri = (planificator *)malloc(sizeof(planificator));

    // se semnaleaza ca exista un planificator
    initializat = 1;

    // se initializeaza campurile specifice planificatorului
    planificator_de_threaduri->numar_io = io;
    planificator_de_threaduri->cuanta_de_timp = time_quantum;
    planificator_de_threaduri->numar_de_threaduri = 0;
    planificator_de_threaduri->numar_de_threaduri_in_coada = 0;
    planificator_de_threaduri->thread_curent = NULL;
    planificator_de_threaduri->threaduri = (thread **)malloc(NUMAR_MAXIM_THREADURI * sizeof(thread *));
    planificator_de_threaduri->coada = (thread **)malloc(NUMAR_MAXIM_THREADURI * sizeof(thread *));

    return 0;
}

// functia realizeaza implementarea algoritmului Round Robin cu prioritati
// (se decide in ce conditii se alege un alt thread pentru rulare)
void actualizare_planificator()
{
    // daca thread-ul curent este singurul ramas si nu isi terminase executia
    // ii dau voie sa ruleze pe procesor
    if (!(planificator_de_threaduri->numar_de_threaduri_in_coada) && planificator_de_threaduri->thread_curent->stare != terminat)
    {
        planificator_de_threaduri->thread_curent->cuanta_ramasa = planificator_de_threaduri->cuanta_de_timp;
        planificator_de_threaduri->thread_curent->stare = ruleaza;
        sem_post(&(planificator_de_threaduri->thread_curent->semafor));
        return;
    }

    // daca mai exista thread-uri in coada
    if (planificator_de_threaduri->numar_de_threaduri_in_coada && planificator_de_threaduri->thread_curent != NULL)
    {
        // daca thread-ul curent este in starea de asteptare, se alege
        // urmatorul din coada pentru rulare (a avut loc o schimbare
        // de context pentru a putea folosi un dispozitiv I/O)
        if (planificator_de_threaduri->thread_curent->stare == asteapta)
        {
            planificator_de_threaduri->thread_curent = primul_din_coada();
            elimina_din_coada();
            planificator_de_threaduri->thread_curent->stare = ruleaza;
            planificator_de_threaduri->thread_curent->cuanta_ramasa = planificator_de_threaduri->cuanta_de_timp;
            sem_post(&(planificator_de_threaduri->thread_curent->semafor));
            return;
        }

        // daca primul element din coada are prioritatea mai mare decat cea a thread-ului curent,
        // cel curent este dat jos de pe procesor si se ruleaza celalalt
        if (planificator_de_threaduri->thread_curent->prioritate < primul_din_coada()->prioritate)
        {
            adauga_in_coada(planificator_de_threaduri->thread_curent);
            planificator_de_threaduri->thread_curent = primul_din_coada();
            elimina_din_coada();
            planificator_de_threaduri->thread_curent->stare = ruleaza;
            planificator_de_threaduri->thread_curent->cuanta_ramasa = planificator_de_threaduri->cuanta_de_timp;
            sem_post(&(planificator_de_threaduri->thread_curent->semafor));
            return;
        }

        // daca thread-ul curent nu mai are cuanta si exista un coada un thread
        // de prioritate mai mare, se alege urmatorul din coada pentru rulare
        if (planificator_de_threaduri->thread_curent->cuanta_ramasa == 0 && planificator_de_threaduri->thread_curent->prioritate == primul_din_coada()->prioritate)
        {
            adauga_in_coada(planificator_de_threaduri->thread_curent);
            planificator_de_threaduri->thread_curent = primul_din_coada();
            elimina_din_coada();
            planificator_de_threaduri->thread_curent->stare = ruleaza;
            planificator_de_threaduri->thread_curent->cuanta_ramasa = planificator_de_threaduri->cuanta_de_timp;
            sem_post(&(planificator_de_threaduri->thread_curent->semafor));
            return;
        }

        // daca thread-ul curent nu mai are cuanta si nu mai exista un coada un thread
        // de prioritate mai mare, se pune din nou pe procesor thread-ul curent
        if (planificator_de_threaduri->thread_curent->cuanta_ramasa == 0 && planificator_de_threaduri->thread_curent->prioritate > primul_din_coada()->prioritate)
        {
            planificator_de_threaduri->thread_curent->stare = ruleaza;
            planificator_de_threaduri->thread_curent->cuanta_ramasa = planificator_de_threaduri->cuanta_de_timp;
            sem_post(&(planificator_de_threaduri->thread_curent->semafor));
            return;
        }

        // daca thread-ul curent mai are cuanta de timp si niciun alt
        // thread nu are prioritate mai mare, se continua executia
        sem_post(&(planificator_de_threaduri->thread_curent->semafor));
    }
}

// functia realizeaza executia instructiunilor specifice thread-ului nou creat
void *rulare_instructiuni_thread_nou(void *arg)
{
    thread *threadul_nou = (thread *)(arg);

    sem_wait(&(threadul_nou->semafor));

    threadul_nou->handler(threadul_nou->prioritate);
    threadul_nou->stare = terminat;

    // daca thread-ul creat este chiar cel curent, se stabileste daca
    // trebuie sa ruleze urmatorul thread din coada (caz in care acesta exista)
    if (planificator_de_threaduri->thread_curent == threadul_nou && planificator_de_threaduri->numar_de_threaduri_in_coada)
    {
        planificator_de_threaduri->thread_curent = primul_din_coada();
        elimina_din_coada();
        planificator_de_threaduri->thread_curent->stare = ruleaza;
        planificator_de_threaduri->thread_curent->cuanta_ramasa = planificator_de_threaduri->cuanta_de_timp;
        sem_post(&(planificator_de_threaduri->thread_curent->semafor));
    }

    return NULL;
}

// instructiunea de "exec" ce scade cuanta thread-ului curent si permite
// rularea urmatorului thread pe procesor daca este cazul
void so_exec(void)
{
    planificator_de_threaduri->thread_curent->cuanta_ramasa--;

    thread *vechiul_thread_curent = planificator_de_threaduri->thread_curent;

    actualizare_planificator();

    sem_wait(&(vechiul_thread_curent->semafor));
}

// functia creeaza un nou thread (aloca memorie si initializeaza)
// apoi il planifica si incepe rularea
// se retuneaza id-ul thread-ului
tid_t so_fork(so_handler *func, unsigned int priority)
{
    // mai intai se verifica daca prioritatea care ar trebui sa i se atribuie thread-ului este valida

    if (priority < 0 || priority > SO_MAX_PRIO)
        return INVALID_TID;

    // daca pointer-ul catre functia handler este NULL, thread-ul nu ar avea ce sa execute
    if (func == NULL)
        return INVALID_TID;

    // se aloca memorie pentru un thread, se initializeaza campurile specifice
    // si porneste thread-ul
    thread *thread_nou = (thread *)malloc(sizeof(thread));

    thread_nou->stare = nou_creat;
    thread_nou->prioritate = priority;
    thread_nou->cuanta_ramasa = planificator_de_threaduri->cuanta_de_timp;
    thread_nou->dispozitiv_io = SO_MAX_NUM_EVENTS; // initial nu exista
    thread_nou->id_thread = INVALID_TID;           // initial fork-ul nu a avut loc
    thread_nou->handler = func;
    sem_init(&(thread_nou->semafor), 0, 0);

    // inceperea efectiva a executiei a thread-ului nou creat
    pthread_create(&(thread_nou->id_thread), NULL, &rulare_instructiuni_thread_nou, (void *)thread_nou);

    // se adauga thread-ul nou in lista de thread-uri a planificatorului
    (planificator_de_threaduri->threaduri)[planificator_de_threaduri->numar_de_threaduri] = thread_nou;
    planificator_de_threaduri->numar_de_threaduri++;

    // se alege ce thread ruleaza pe procesor
    // daca nu exista un thread curent, el devine cel nou creat
    // si se porneste executia lui
    // altfel, thread-ul este pus in coada si isi asteapta executia
    if (planificator_de_threaduri->thread_curent == NULL)
    {
        planificator_de_threaduri->thread_curent = thread_nou;
        planificator_de_threaduri->thread_curent->stare = ruleaza;
        planificator_de_threaduri->thread_curent->cuanta_ramasa = planificator_de_threaduri->cuanta_de_timp;
        sem_post(&(planificator_de_threaduri->thread_curent->semafor));
    }
    else
    {
        thread_nou->stare = gata;
        adauga_in_coada(thread_nou);
        so_exec();
    }

    return thread_nou->id_thread;
}

// functia determina punerea thread-ului curent in asteptarea
// dispozitivului io de care are nevoie pentru executie si se continua
// procesul de rulare
int so_wait(unsigned int io)
{
    if (io < 0 || io >= planificator_de_threaduri->numar_io)
        return -1;

    planificator_de_threaduri->thread_curent->stare = asteapta;
    planificator_de_threaduri->thread_curent->dispozitiv_io = io;

    so_exec();

    return 0;
}

// functia identifica ce thread-uri asteapta utilizarea unui anumit dispozitiv
// I/O, acestea urmand sa fie adaugate in coada; dupa aceea, se continua
// procesul de rulare
// se retuneaza numarul thread-urilor gasite
int so_signal(unsigned int io)
{
    if (io < 0 || io >= planificator_de_threaduri->numar_io)
        return -1;

    int i, nr = 0;
    for (i = 0; i < planificator_de_threaduri->numar_de_threaduri; i++)
        if ((planificator_de_threaduri->threaduri)[i]->dispozitiv_io == io && (planificator_de_threaduri->threaduri)[i]->stare == asteapta)
        {
            (planificator_de_threaduri->threaduri)[i]->stare = gata;
            // rularea se va face cu ajutorul dispozitivului asteptat de thread
            // evitandu-se executia de mai multe ori a acestuia
            (planificator_de_threaduri->threaduri)[i]->dispozitiv_io = SO_MAX_NUM_EVENTS;
            adauga_in_coada((planificator_de_threaduri->threaduri)[i]);
            nr++;
        }
    
    so_exec();

    return nr;
}

// functia permite thread-uriloe create sa isi termine executia si
// elibereaza toata memoria alocata 
void so_end(void)
{
    if (!initializat)
        return;

    int i;

    for (i = 0; i < planificator_de_threaduri->numar_de_threaduri; i++)
        pthread_join(planificator_de_threaduri->threaduri[i]->id_thread, NULL);

    for (i = 0; i < planificator_de_threaduri->numar_de_threaduri; i++)
        sem_destroy(&((planificator_de_threaduri->threaduri)[i]->semafor));

    for (i = 0; i < planificator_de_threaduri->numar_de_threaduri; i++)
        free((planificator_de_threaduri->threaduri)[i]);

    free(planificator_de_threaduri->threaduri);
    free(planificator_de_threaduri->coada);

    free(planificator_de_threaduri);

    planificator_de_threaduri = NULL;
    initializat = 0;
}