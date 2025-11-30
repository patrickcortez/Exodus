def calculate(x,y) -> bool:
    
    x = x * y
    
    if x%2 == 0:
        return True
    else:
        return False
    
if __name__ == "__main__":
        print("Enter your Numba's:")
        x = int(input("Num1:"))
        y = int(input("Num2:"))
        
        is_even = calculate(x,y)
        
        if is_even:
            print("It is Even :)")
        else:
            print("It is not even :<")