use rand::Rng;
use std::{cmp::Ordering, io};

fn main() {
    println!("猜数游戏！");
    let secret_number = rand::thread_rng().gen_range(1..100);
    println!("神秘数字是：");

    let mut guess = String::new();
    io::stdin().read_line(&mut guess).expect("无法读取行");
    let guess: i32 = guess.trim().parse().expect("");

    match guess.cmp(&secret_number) {
        Ordering::Less => println!("小了"),
        Ordering::Greater => println!("大了"),
        Ordering::Equal => println!("你赢了"),
    }
}
