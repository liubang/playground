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
