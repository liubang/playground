use serde::Deserialize;
use serde::Serialize;

#[derive(Serialize, Deserialize)]
pub struct CommonResult<T> {
    pub code: i64,
    pub message: String,
    pub data: T,
}
