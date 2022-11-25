//=====================================================================
//
// main.tsx -
//
// Created by liubang on 2022/10/30 23:58
// Last Modified: 2022/10/30 23:58
//
//=====================================================================

import React from "react";

import ReactDOM from "react-dom/client";
// 样式初始化一般放在最前
import "reset-css";

// UI 框架的样式

import "@/assets/styles/global.scss";

// 组件的样式

import App from "./App";
import { BrowserRouter } from "react-router-dom";

ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(
  <React.StrictMode>
    <BrowserRouter>
      <App />
    </BrowserRouter>
  </React.StrictMode>
);
