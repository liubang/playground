// Copyright (c) 2022 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2022/12/23 15:19

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
