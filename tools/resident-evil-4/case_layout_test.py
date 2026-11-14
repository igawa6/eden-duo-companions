# SPDX-License-Identifier: GPL-3.0-or-later
import unittest
from case_layout import CAPACITIES, NAME_Y, layout


class LayoutTests(unittest.TestCase):
    def test_all_native_capacities(self):
        for capacity, expected in zip(CAPACITIES, ((120,20,128),(108,26,110),(96,44,104),(80,20,168))):
            cell,x,y=layout(*capacity)
            self.assertEqual((cell,x,y),expected)
            self.assertTrue(all(v%2==0 for v in (cell,x,y)))
            cols,rows=capacity
            self.assertGreaterEqual(x,20);self.assertGreaterEqual(y,104)
            self.assertLessEqual(x+cols*cell,1220);self.assertLessEqual(y+rows*cell,872)
            self.assertLessEqual(y+rows*cell+12,NAME_Y-4)
            self.assertTrue(cols*(cell+2)>1200 or rows*(cell+2)>768)

    def test_tap_round_trip(self):
        for cols,rows in CAPACITIES:
            cell,gx,gy=layout(cols,rows)
            for y in range(rows):
                for x in range(cols):
                    for dx,dy in ((0,0),(cell//2,cell//2),(cell-1,cell-1)):
                        px,py=gx+x*cell+dx,gy+y*cell+dy
                        payload=((px-20)<<32)|(py-104)
                        rx,ry=20+(payload>>32),104+(payload&0xffffffff)
                        self.assertEqual(((rx-gx)//cell,(ry-gy)//cell),(x,y))


if __name__=='__main__':unittest.main()
