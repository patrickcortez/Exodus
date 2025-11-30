#include <iostream>
#include "lib/program.h"

using namespace std;

enum Difficulty{
    LOW=1,
    MID=2,
    HIGH=3
};

int choosediff(){
    bool valid = false;
    int res;

    while(valid != true){
        char choice;
        cout << "Choose Your Difficulty: [Easy{E},Medium{M},Hard{H}]";
        cin >> choice;

        switch(choice){
            case 'E':
            case 'e':
                return res = LOW;
                break;
            case 'M':
            case 'm':
                return res = MID;
                break;
            case 'H':
            case 'h':
                return res = HIGH;
                break;
            default:
                continue;
                break;
        }
    }
}

int main() {
    Program* prog = new Program;

    prog->setDiff(choosediff()); //set Game Difficulty
    prog->run(); //Run main Game

    //Main game init
}