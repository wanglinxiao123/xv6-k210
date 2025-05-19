// 定义串口通信接口
trait SerialPair: core::fmt::Write {
	fn getchar(&mut self) ->Option<u8>;
	fn putchar(&mut self, c: u8);
}

// 串口发送和接收的接口
use embedded_hal::serial::{
	Write, Read, 
};
// 永远不会失败的错误类型
use core::convert::Infallible;


// 实现串口数据收发
struct Serial<T, R>(T, R);
impl<T, R> SerialPair for Serial<T, R> 
where 
	T: Write<u8, Error = Infallible> + Send + 'static, 
	R: Read<u8, Error = Infallible> + Send + 'static, 
{
	fn getchar(&mut self) ->Option<u8> {
		match self.1.try_read() {
			Ok(c) => Some(c), 
			Err(_) => None, 
		}
	}

	fn putchar(&mut self, c: u8) {
		while let Err(_) = self.0.try_write(c) {}
	}
}

impl<T, R> core::fmt::Write for Serial<T, R>
where 
	T: Write<u8, Error = Infallible> + Send + 'static, 
	R: Read<u8, Error = Infallible> + Send + 'static, 
	Serial<T, R>: SerialPair, 
{
	fn write_str(&mut self, s: &str) ->core::fmt::Result {
		for c in s.bytes() {
			self.putchar(c as u8);
		}
		Ok(())
	}
}

use lazy_static::lazy_static;
use spin::Mutex;

extern crate alloc;
use alloc::boxed::Box;
lazy_static! {
	static ref UARTHS: Mutex<Option<Box<dyn SerialPair + Send>>> = 
			Mutex::new(None);
}

pub fn init(ser: k210_hal::serial::Serial<k210_hal::pac::UARTHS>) {
	let (tx, rx) = ser.split();

	*UARTHS.lock() = Some(Box::new(Serial(tx, rx)));

	crate::println!("serial init");
}

pub fn getchar() ->Option<u8> {
	let mut _uarths = UARTHS.lock();	// acquire lock 

	match _uarths.as_mut() {
		Some(ser) => ser.getchar(), 
		_ => None, 
	}
}

// 串口输出一个字节
pub fn putchar(c: u8) {
	let mut _uarths = UARTHS.lock();
	if let Some(ser) = _uarths.as_mut() {
		ser.putchar(c);
	}
}

pub fn print(args: core::fmt::Arguments) {
	// write_fmt() is provided by `Write`, so there's 
	// no need that we implement it. 
	let mut _uarths = UARTHS.lock();	// acquire lock 

	if let Some(ser) = _uarths.as_mut() {
		ser.write_fmt(args).unwrap();
	}
}

#[macro_export]
macro_rules! print {
	($fmt: literal $(, $($arg: tt)+)?) => {
		$crate::serial::print(format_args!($fmt $(, $($arg)+)?));
	}
}

#[macro_export]
macro_rules! println {
	($fmt: literal $(, $($arg: tt)+)?) => {
		$crate::serial::print(format_args!(concat!($fmt, "\n") $(, $($arg)+)?));
	}
}
