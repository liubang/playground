enum Direction {
    East,
    West,
    North,
    South,
}

fn demo1() {
    let dire = Direction::South;
    match dire {
        Direction::East => println!("East"),
        Direction::North | Direction::South => {
            println!("South or North");
        }
        _ => println!("West"),
    };
}

enum IpAddr {
    Ipv4,
    Ipv6,
}

fn demo2() {
    let ip1 = IpAddr::Ipv6;
    let ip_str = match ip1 {
        IpAddr::Ipv4 => "127.0.0.1",
        _ => "::1",
    };
    println!("ip_str: {}", ip_str)
}

#[derive()]
enum FooBar {
    Foo,
    Bar,
}

fn demo3() {
    let v = vec![FooBar::Foo, FooBar::Bar, FooBar::Foo];
    // 这里匹配了所有的结果
    let mut m = v.iter().filter(|x| matches!(x, FooBar::Foo));
    loop {
        match m.next() {
            None => break,
            Some(_el) => {
                println!("OK")
            }
        }
    }
}

fn main() {
    demo1();
    demo2();
    demo3();
}
