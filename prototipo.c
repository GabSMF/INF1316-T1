
/*---- Includes e Definições ----*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>


/*---- Pistas ----*/
#define PISTA_E_03 3
#define PISTA_E_06 6
#define PISTA_W_18 18
#define PISTA_W_27 27


/*---- Struct das Aeronaves ----*/
typedef struct 
{
    char lado; // Lado de entrada (E ou W)
    int pista; // Pista preferida para pouso
    int atraso; // Tempo de atraso em segundos
    float posY; // Coordenada Y
    float posX; // Coordenada X
} Aviao;


/*---- Globais ----*/
char ladoEntrada[2] = {'E','W'};
int pistasEast[2] = {PISTA_E_03, PISTA_E_06};
int pistasWest[2] = {PISTA_W_18, PISTA_W_27};


/*---- Funções de Criação de Aeronave ----*/


char geraEntrada(){
    char entrada = ladoEntrada[rand() % 2];
    return entrada;
}

float geraCoordenadaY()
{
    double coordenadaY = (double)rand() / RAND_MAX;
    return coordenadaY;
}

int geraSorteioAtraso(){
    int atraso = rand() % 11;
    return atraso;
}

int geraPistaPreferida(char entrada){
    int pistaPreferida;
    if(entrada == 'E'){
        pistaPreferida = pistasEast[rand() % 2];
    }
    else{
        pistaPreferida = pistasWest[rand() % 2];
    }
    return pistaPreferida;
}

Aviao criaAviao(){
    Aviao A;

    A.atraso = geraSorteioAtraso();
    A.lado = geraEntrada();
    A.pista = geraPistaPreferida(A.lado);
    A.posY = geraCoordenadaY();
    return A;
}


int main(void){
    srand(time(NULL));

    Aviao aviao1 = criaAviao();
    printf("Avião 1: %c Pista-%d %dsecs %.2f\n", aviao1.lado, aviao1.pista, aviao1.atraso, aviao1.posY);

    Aviao aviao2 = criaAviao();
    printf("Avião 2: %c Pista-%d %dsecs %.2f\n", aviao2.lado, aviao2.pista, aviao2.atraso, aviao2.posY);

    Aviao aviao3 = criaAviao();
    printf("Avião 3: %c Pista-%d %dsecs %.2f\n", aviao3.lado, aviao3.pista, aviao3.atraso, aviao3.posY);
}


