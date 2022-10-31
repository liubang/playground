import { BrowserRouter, Route, Routes } from "react-router-dom";
import App from "../App";
import Home from "../views/Home";
import About from "../views/About";

const baseRouter = () => (
  <BrowserRouter>
    <Routes>
      <Route path="/" element={<App />}>
        <Route path="/home" element={<Home />}></Route>
        <Route path="/about" element={<About />}></Route>
      </Route>
    </Routes>
  </BrowserRouter>
);

export default baseRouter;
