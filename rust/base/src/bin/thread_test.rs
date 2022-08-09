use std::{thread, time::Duration};

fn main() {
    let handler = thread::spawn(|| {
        for i in 1..1000 {
            println!("the number {} in thread spawn", i);
            thread::sleep(Duration::from_millis(1))
        }
    });

    for i in 1..10 {
        println!("the number {} in main thread", i);
        thread::sleep(Duration::from_millis(1))
    }

    handler.join().unwrap()
}
