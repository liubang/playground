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
// Created: 2022/10/31 00:10

//=====================================================================
//
// App.tsx -
//
// Created by liubang on 2022/10/30 23:58
// Last Modified: 2022/10/30 23:58
//
//=====================================================================

import { useState } from "react";
import { Button } from "antd";
import { UpCircleOutlined } from "@ant-design/icons";
import { Outlet } from "react-router-dom";

function App() {
  const [count, setCount] = useState(0);

  return (
    <div className="App">
      {/*顶级组件
      <br />
      <UpCircleOutlined style={{ fontSize: "30px", color: "red" }} />
      <Button type="primary">我们的按钮</Button> */}
      {/* 占位符，类似于窗口，用来展示组件的，有点像vue中的router-view */}
      <Outlet></Outlet>
    </div>
  );
}

export default App;
