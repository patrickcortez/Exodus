#include <iostream>

using namespace std;

int main(){
string name;

cout << "Hello World!" << endl;

cout << "Whats your name?"; 

cin >> name;

if(name != "Patrick"){

cout << "Wrong name buddy" <<endl;
return 1;
}else{

cout << "welcome master =)" << endl;
return 0;
}

return 0;
}