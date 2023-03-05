//=====================================================================
//
// App.tsx -
//
// Created by liubang on 2022/10/30 23:58
// Last Modified: 2022/10/30 23:58
//
//=====================================================================

import react from "react";
import { Button } from "antd";
import * as icons from "@ant-design/icons";
import { useRoutes, Link } from "react-router-dom";
import router from "@/router";

function App() {
  const [count, setCount] = react.useState(0);
  const outlet = useRoutes(router);

  return (
    <div className="App">
      {outlet}
    </div>
  );
}

export default App;
