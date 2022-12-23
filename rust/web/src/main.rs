mod handlers;

use actix_web::App;
use actix_web::HttpServer;

#[actix_web::main]
async fn main() -> std::io::Result<()> {
    HttpServer::new(|| App::new().service(handlers::index::index))
        .bind(("127.0.0.1", 8801))?
        .run()
        .await
}
