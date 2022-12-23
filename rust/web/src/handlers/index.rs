use actix_web::get;
use actix_web::web;
use actix_web::Responder;
use actix_web::Result;

use super::model::CommonResult;

#[get("/index.html")]
async fn index() -> Result<impl Responder> {
    let obj = CommonResult {
        code: 0,
        message: "OK".to_string(),
        data: "Ok".to_string(),
    };

    Ok(web::Json(obj))
}
