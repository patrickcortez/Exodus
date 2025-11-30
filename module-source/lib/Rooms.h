#ifndef ROOMS_H
#define ROOMS_H

#include <iostream>

using namespace std;

struct Room{
    string name,desc;
    Room* North = nullptr, * South = nullptr, * East = nullptr, * West = nullptr;
    bool locked;
    Room(string pname,string pdesc,bool plock): name(pname), desc(pdesc), locked(plock){}

    void setSibling(Room* N = nullptr,Room* S= nullptr, Room* E=nullptr, Room* W=nullptr){

        if(N != nullptr){
            this->North = N;
        }

        if(S != nullptr){
            this->South = S;
        }

        if(W != nullptr){
            this->West = W;
        }

        if(E != nullptr){
            this->East = E;
        }
    }
};

#endif