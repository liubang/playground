#! /usr/bin/env python
# -*- coding: utf-8 -*-

import asyncio

from crawl4ai import AsyncWebCrawler

async def main():
    async with AsyncWebCrawler(verbose=True) as crawler:
        result = await crawler.arun(url="https://www.baidu.com")
        print(result.markdown)

if __name__ == "__main__":
    asyncio.run(main())
