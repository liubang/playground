// 类似于c++的命名空间
// use std::io;

fn main() {
    println!("Guess the number!");
    println!("Please input your guess.");

    // mut 是非const类型，可变
    let mut guess = String::new();
    std::io::stdin()
        .read_line(&mut guess)
        .expect("Failed to read line");

    println!("You guessed: {}", guess);
}
