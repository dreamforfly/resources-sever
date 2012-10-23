FLAGS = -Wall -pedantic -pthread -O3

all: klient serwer

serwer: serwer.c common.h err.o
	gcc $(FLAGS) -o serwer serwer.c err.o

klient: klient.c common.h err.o
	gcc $(FLAGS) -o klient klient.c err.o

err.o: err.c err.h
	gcc $(FLAGS) -c -o err.o err.c
