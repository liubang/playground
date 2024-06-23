// Copyright (c) 2024 The Authors. All rights reserved.
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

///////////////////////////////////////////////////////////////////////////////////////////////////
//// 组件形式的路由写法
//
// import { BrowserRouter, Route, Routes } from "react-router-dom";
// import App from "../App";
// import Home from "../views/Home";
// import About from "../views/About";
//
// const baseRouter = () => (
//   <BrowserRouter>
//     <Routes>
//       <Route path="/" element={<App />}>
//         <Route path="/home" element={<Home />}></Route>
//         <Route path="/about" element={<About />}></Route>
//       </Route>
//     </Routes>
//   </BrowserRouter>
// );
//
// export default baseRouter;

///////////////////////////////////////////////////////////////////////////////////////////////////
//
//另一种写法

import React, { lazy } from "react";
import Home from "@/views/Home";
// import About from "@/views/About";
// 重定向组件
import { Navigate } from "react-router-dom";

// 懒加载模式
const About = lazy(() => import("@/views/About"));

const withLoadingComponent = (comp: JSX.Element) => (
  <React.Suspense fallback={<div>Loading...</div>}>{comp}</React.Suspense>
);

const routes = [
  {
    path: "/",
    element: <Navigate to="/home" />,
  },
  {
    path: "/home",
    element: <Home />,
  },
  {
    path: "/about",
    element: withLoadingComponent(<About />),
  },
];

export default routes;
