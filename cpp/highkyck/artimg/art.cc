#include <CImg.h>
#include <fstream>
#include <iostream>

constexpr char ASCII_LIST[] =
    "$@B%8&WM#*oahkbdpqwmZO0QLCJUYXzcvunxrjft/|()1{}[]?-_+~<>i!lI;:,\"^`'. ";


using namespace cimg_library;

CImg<> rgb2gray(CImg<> color_img)
{
    // create a gray image
    CImg<> gray_img(color_img.width(), color_img.height(), 1, 1, 0);

    // for all pixels x, y in image
    cimg_forXY(color_img, x, y)
    {
        int R = (int)color_img(x, y, 0, 0);
        int G = (int)color_img(x, y, 0, 1);
        int B = (int)color_img(x, y, 0, 2);

        int gray_val         = (int)(0.299 * R + 0.587 * G + 0.114 * B);
        gray_img(x, y, 0, 0) = gray_val;
    }

    gray_img.normalize(0, 255);
    return gray_img;
}

void print_gray2ascii(CImg<> gray_img, const char* file_name, int w, int h)
{
    std::ofstream out(file_name);
    gray_img.resize(w, h);

    cimg_forY(gray_img, y)
    {
        cimg_forX(gray_img, x)
        {
            int val = gray_img(x, y, 0, 0) / sizeof(ASCII_LIST);
            std::cout << ASCII_LIST[val];
        }
        std::cout << endl;
    }
    out.close();
}

int main(int argc, char* argv[])
{
    cimg::imagemagick_path("");
    CImg<> img("");
    img                   = rgb2gray(img);
    const char* file_name = "out.txt";
    print_gray2ascii(img, file_name, 200, 800);
    return 0;
}
