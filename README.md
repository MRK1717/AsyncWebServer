# AsyncWebServer

## Prezentare generală
AsyncWebServer este un server web asincron dezvoltat în C, realizat ca proiect de facultate. 
Scopul său este de a demonstra concepte de I/O non-blocant, programare evenimentială și programare de rețea.

## Funcționalități
- Gestionarea asincronă a cererilor HTTP
- Suport pentru multiple conexiuni simultane
- Rutare de bază și servire de fișiere statice
- Mecanisme de logging și tratarea erorilor

## Cerințe
- Compilator C (de ex. GCC)
- Sistem de operare compatibil POSIX (Linux, Unix, macOS)
- Biblioteca pthread (pentru multi-threading)

## Compilare
Utilizează Makefile-ul inclus:
```bash
make
