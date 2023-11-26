use num::complex::Complex;

pub fn add(a: i32, b: i32) -> i32 {
    return a + b;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_add() {
        assert_eq!(add(1, 2), 3);
    }
}

fn main() {
    println!("Hello, world!");

    {
        let x = 10;
        println!("the value of x is: {}", x);

        let mut y = 15;
        println!("this value of y is: {}", y);

        y = 20;
        println!("this value of y is: {}", y);

        let _unused = 10;
        // warning: unused variable: `unused`
        let _unused = 10;
    }

    {
        let (a, mut b): (bool, bool) = (true, false);
        println!("a = {:?}, b = {:?}", a, b);
        b = true;
        assert_eq!(a, b);
    }

    {
        struct Struct {
            e: i32,
        }

        let (a, b, c, d, e);
        (a, b) = (1, 2);
        [c, .., d, _] = [1, 2, 3, 4, 5];
        Struct { e, .. } = Struct { e: 5 };

        assert_eq!([1, 2, 1, 4, 5], [a, b, c, d, e]);
    }

    {
        const MAX_OPTIONS: u32 = 100_000;
        println!("MAX_OPTIONS = {}", MAX_OPTIONS)
    }

    {
        // shadowing
        let x = 10;
        let x = x + 5;
        {
            let x = x * 3;
            println!("x = {}", x);
        }
        println!("x = {}", x);
    }

    {
        let x = (-42.0_f32).sqrt();
        if x.is_nan() {
            println!("x is NAN")
        }
    }

    {
        // [1, 5)
        for i in 1..5 {
            println!("i : {}", i)
        }

        println!("===================");

        // [1, 5]
        for i in 1..=5 {
            println!("i : {}", i)
        }
    }

    {
        let a = Complex { re: 2.1, im: -1.2 };
        let b = Complex::new(11.1, 22.2);
        let result = a + b;
        println!("re: {}, im: {}i", result.re, result.im)
    }

    {
        let c = '中';
        println!("'中'占用了{}个字节", std::mem::size_of_val(&c));
    }

    {
        let (a, b) = (10, 20);
        println!("a + b = {}", add(a, b));
    }

    {
        let s1 = String::from("hello");
        // 这里类似于 c++ 中的 std::move 操作
        let s2 = s1;
        println!("s2 is {}", s2);
        // 此时 s1 将会失效，字符串的所有权已经交给了 s2
        // println!("s1 is {}", s1);
    }

    {
        struct Cacher<T>
        where
            T: Fn(u32) -> u32,
        {
            query: T,
            value: Option<u32>,
        }

        // impl
        impl<T> Cacher<T>
        where
            T: Fn(u32) -> u32,
        {
            fn new(query: T) -> Cacher<T> {
                Cacher {
                    query: query,
                    value: None,
                }
            }

            fn value(&mut self, arg: u32) -> u32 {
                match self.value {
                    Some(v) => v,
                    None => {
                        let v = (self.query)(arg);
                        self.value = Some(v);
                        return v;
                    }
                }
            }
        }
    }
}
