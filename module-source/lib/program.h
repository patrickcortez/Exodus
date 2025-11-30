#ifndef PROGRAM_H
#define PROGRAM_H

#include <iostream>
#include <vector>
#include "Rooms.h"

using namespace std;

class Program{

    private:
        Room* currentroom = nullptr;
        vector<Room> rooms;
        int diff = 0;

    public:

        int setDiff(int level){
            this->diff = level;
        }

        void setRooms();

        void run();

};

void Program::setRooms(){
    Room entry("House Entrance","A cozy room with the front door closed and a huge window besides it",false);
    Room hallway("Hallway","A long hallway with paintings on the wall, it connects to more rooms",false);


    rooms.push_back(entry);
    rooms.push_back(hallway);

    rooms[0].setSibling(NULL,&hallway);

}

void Program::run(){
    setRooms();
    currentroom = &rooms[0];

    cout << "You are in: " << currentroom->name <<endl;
    cout << currentroom->desc << endl;
}

#endif