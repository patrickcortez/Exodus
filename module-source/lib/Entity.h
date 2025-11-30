#ifndef ENTITY_H
#define ENTITY_H

#include <iostream>

using namespace std;

class Entity{
    private:
        string name, desc;
        int hp;
    public:
        
        Entity(string pname,string pdesc,int health) : name(pname), desc(pdesc), hp(health){} //Initialize Entity
        
        

};

#endif // !ENTITY_H