//=====================================================================
//
// index.tsx -
//
// Created by liubang on 2022/11/26 01:04
// Last Modified: 2022/11/26 01:04
//
//=====================================================================
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
