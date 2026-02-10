#include <msp430.h>
#include <stdint.h>

/* ------------ SSD1306 basics ------------ */
#define SSD1306_ADDR   0x3C
#define CTRL_CMD       0x00  /* Co=0, D/C#=0 (command) */
#define CTRL_DATA      0x40  /* Co=0, D/C#=1 (data)    */
#define BOARD_W 8            /* 8 columns */
#define BOARD_H 16           /* 16 rows */

/* Piece types for 7-bag system */
#define PIECE_I 0
#define PIECE_J 1
#define PIECE_L 2
#define PIECE_O 3
#define PIECE_S 4
#define PIECE_T 5
#define PIECE_Z 6

/* 7-bag system state */
static uint8_t bag[7];         /* Current bag of 7 pieces */
static uint8_t bag_index = 7;  /* Index in current bag (7 = need new bag) */
static uint8_t preview[6];     /* Next 6 pieces to show */
static uint8_t current_piece_type = PIECE_L;

/* Piece state */
static int8_t  piece_x = 3;
static int8_t  piece_y = 0;
static uint8_t rot      = 0;
static uint16_t drop_counter = 0;
static uint8_t  slow_divider = 0;

static uint8_t board[BOARD_H][BOARD_W];  /* 0 = empty, 1 = filled */

/* Forward declarations */
static void ssd_set_page_col(uint8_t page, uint8_t col);
static void draw_piece(void);
static void clear_piece(void);
static uint8_t can_rotate(uint8_t new_rot);
static uint8_t can_rotate_at_offset(uint8_t new_rot, int8_t x_offset);

/* Offsets: {dx, dy} in grid cells */
typedef struct {
    int8_t dx;
    int8_t dy;
} CellOffset;

/* All 7 tetromino offsets: [piece_type][rotation][block] */
static const CellOffset PIECE_OFFSETS[7][4][4] = {
    /* PIECE_I */
    {
        { {0,0}, {0,1}, {0,2}, {0,3} },  /* vertical */
        { {0,0}, {1,0}, {2,0}, {3,0} },  /* horizontal */
        { {0,0}, {0,1}, {0,2}, {0,3} },  /* vertical */
        { {0,0}, {1,0}, {2,0}, {3,0} }   /* horizontal */
    },
    /* PIECE_J */
    {
        { {1,0}, {1,1}, {1,2}, {0,2} },
        { {0,0}, {0,1}, {1,1}, {2,1} },
        { {0,0}, {1,0}, {0,1}, {0,2} },
        { {0,0}, {1,0}, {2,0}, {2,1} }
    },
    /* PIECE_L */
    {
        { {0,0}, {0,1}, {0,2}, {1,2} },
        { {0,0}, {1,0}, {2,0}, {0,1} },
        { {0,0}, {1,0}, {1,1}, {1,2} },
        { {2,0}, {0,1}, {1,1}, {2,1} }
    },
    /* PIECE_O */
    {
        { {0,0}, {1,0}, {0,1}, {1,1} },
        { {0,0}, {1,0}, {0,1}, {1,1} },
        { {0,0}, {1,0}, {0,1}, {1,1} },
        { {0,0}, {1,0}, {0,1}, {1,1} }
    },
    /* PIECE_S */
    {
        { {1,0}, {2,0}, {0,1}, {1,1} },
        { {0,0}, {0,1}, {1,1}, {1,2} },
        { {1,0}, {2,0}, {0,1}, {1,1} },
        { {0,0}, {0,1}, {1,1}, {1,2} }
    },
    /* PIECE_T */
    {
        { {1,0}, {0,1}, {1,1}, {2,1} },
        { {1,0}, {1,1}, {2,1}, {1,2} },
        { {0,0}, {1,0}, {2,0}, {1,1} },
        { {0,0}, {0,1}, {1,1}, {0,2} }
    },
    /* PIECE_Z */
    {
        { {0,0}, {1,0}, {1,1}, {2,1} },
        { {1,0}, {0,1}, {1,1}, {0,2} },
        { {0,0}, {1,0}, {1,1}, {2,1} },
        { {1,0}, {0,1}, {1,1}, {0,2} }
    }
};

/* ------------ I2C helper ------------ */
static void i2c_write(uint8_t ctrl, const uint8_t *bytes, uint8_t len)
{
    uint8_t i;

    UCB0I2CSA = SSD1306_ADDR;
    UCB0CTLW0 |= UCTR | UCTXSTT;
    while (UCB0CTLW0 & UCTXSTT) { }

    while (!(UCB0IFG & UCTXIFG)) { }
    UCB0TXBUF = ctrl;

    for (i = 0; i < len; i++)
    {
        while (!(UCB0IFG & UCTXIFG)) { }
        UCB0TXBUF = bytes[i];
    }

    while (!(UCB0IFG & UCTXIFG)) { }
    UCB0CTLW0 |= UCTXSTP;
    while (UCB0CTLW0 & UCTXSTP) { }
}

static void ssd_cmd(const uint8_t *cmds, uint8_t n)
{
    i2c_write(CTRL_CMD, cmds, n);
}

static void ssd_data(const uint8_t *data, uint8_t n)
{
    i2c_write(CTRL_DATA, data, n);
}

/* ------------ SSD1306 init ------------ */
static void ssd_init(void)
{
    uint8_t seq1[1];
    uint8_t seq2[22];
    uint8_t on[1];

    seq1[0] = 0xAE;
    ssd_cmd(seq1, 1);

    seq2[0]  = 0xD5;
    seq2[1]  = 0x80;
    seq2[2]  = 0xA8;
    seq2[3]  = 0x3F;
    seq2[4]  = 0xD3;
    seq2[5]  = 0x00;
    seq2[6]  = 0x40;
    seq2[7]  = 0x8D;
    seq2[8]  = 0x14;
    seq2[9]  = 0x20;
    seq2[10] = 0x02;
    seq2[11] = 0xA1;
    seq2[12] = 0xC8;
    seq2[13] = 0xDA;
    seq2[14] = 0x12;
    seq2[15] = 0x81;
    seq2[16] = 0x7F;
    seq2[17] = 0xD9;
    seq2[18] = 0xF1;
    seq2[19] = 0xDB;
    seq2[20] = 0x40;
    seq2[21] = 0xA4;
    ssd_cmd(seq2, 22);

    seq1[0] = 0xA6;
    ssd_cmd(seq1, 1);

    on[0] = 0xAF;
    ssd_cmd(on, 1);
}

/* ------------ Page + column ------------ */
static void ssd_set_page_col(uint8_t page, uint8_t col)
{
    uint8_t cmd[1];

    cmd[0] = (uint8_t)(0xB0 | (page & 0x07));
    ssd_cmd(cmd, 1);

    cmd[0] = (uint8_t)(0x00 | (col & 0x0F));
    ssd_cmd(cmd, 1);

    cmd[0] = (uint8_t)(0x10 | ((col >> 4) & 0x0F));
    ssd_cmd(cmd, 1);
}

/* ------------ Clear full screen ------------ */
static void clear_screen(void)
{
    uint8_t page;
    uint8_t data[1];
    uint8_t i;

    data[0] = 0x00;

    for (page = 0; page < 8; page++)
    {
        ssd_set_page_col(page, 0);
        for (i = 0; i < 128; i++)
        {
            ssd_data(data, 1);
        }
    }
}

/* ------------ Draw / clear 8x8 cell ------------ */
static void draw_block_cell(uint8_t gx, uint8_t gy)
{
    uint8_t page = gx;
    uint8_t col_start = (uint8_t)(gy * 8);
    uint8_t data[1] = { 0xFF };
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        ssd_set_page_col(page, (uint8_t)(col_start + i));
        ssd_data(data, 1);
    }
}

static void clear_block_cell(uint8_t gx, uint8_t gy)
{
    uint8_t page = gx;
    uint8_t col_start = (uint8_t)(gy * 8);
    uint8_t data[1] = { 0x00 };
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        ssd_set_page_col(page, (uint8_t)(col_start + i));
        ssd_data(data, 1);
    }
}

/* ------------ Draw / clear current piece ------------ */
static void draw_piece(void)
{
    uint8_t i;
    for (i = 0; i < 4; i++)
    {
        int8_t gx = piece_x + PIECE_OFFSETS[current_piece_type][rot][i].dx;
        int8_t gy = piece_y + PIECE_OFFSETS[current_piece_type][rot][i].dy;
        if (gx >= 0 && gx < BOARD_W && gy >= 0 && gy < BOARD_H)
        {
            draw_block_cell((uint8_t)gx, (uint8_t)gy);
        }
    }
}

static void clear_piece(void)
{
    uint8_t i;
    for (i = 0; i < 4; i++)
    {
        int8_t gx = piece_x + PIECE_OFFSETS[current_piece_type][rot][i].dx;
        int8_t gy = piece_y + PIECE_OFFSETS[current_piece_type][rot][i].dy;
        if (gx >= 0 && gx < BOARD_W && gy >= 0 && gy < BOARD_H)
        {
            clear_block_cell((uint8_t)gx, (uint8_t)gy);
        }
    }
}

/* ------------ Init MSP430 ------------ */
static void init_clock(void)
{
    FRCTL0 = FRCTLPW | NWAITS_1;

    CSCTL0_H = CSKEY_H;
    CSCTL1   = DCOFSEL_4 | DCORSEL;
    CSCTL2   = SELA__VLOCLK | SELS__DCOCLK | SELM__DCOCLK;
    CSCTL3   = DIVA__1 | DIVS__1 | DIVM__1;
    CSCTL0_H = 0;
}

static void init_gpio(void)
{
    PM5CTL0 &= ~LOCKLPM5;

    /* I2C pins: P1.6 SDA, P1.7 SCL */
    P1SEL0 |= BIT6 | BIT7;
    P1SEL1 &= ~(BIT6 | BIT7);

    /* P2.3: rotate */
    P2DIR &= ~BIT3;
    P2REN |= BIT3;
    P2OUT |= BIT3;

    /* P3.1: move left */
    P3DIR &= ~BIT1;
    P3REN |= BIT1;
    P3OUT |= BIT1;

    /* P1.1: right */
    P1DIR &= ~BIT1;
    P1REN |= BIT1;
    P1OUT |= BIT1;

    /* P1.2: left */
    P1DIR &= ~BIT2;
    P1REN |= BIT2;
    P1OUT |= BIT2;
}

static void init_i2c(void)
{
    UCB0CTLW0 = UCSWRST;
    UCB0CTLW0 |= UCMST | UCMODE_3 | UCSYNC | UCSSEL__SMCLK;
    UCB0BRW = 160;   /* ~100 kHz at 16 MHz */
    UCB0CTLW0 &= ~UCSWRST;
}

/* ------------ Movement / rotation checks ------------ */
static uint8_t can_move_down(void)
{
    uint8_t i;
    for (i = 0; i < 4; i++)
    {
        int8_t gx = piece_x + PIECE_OFFSETS[current_piece_type][rot][i].dx;
        int8_t gy = piece_y + PIECE_OFFSETS[current_piece_type][rot][i].dy + 1;

        if (gx < 0 || gx >= BOARD_W || gy >= BOARD_H)
            return 0;

        if (gy >= 0 && board[gy][gx] != 0)
            return 0;
    }
    return 1;
}

static uint8_t can_move_left(void)
{
    uint8_t i;
    for (i = 0; i < 4; i++)
    {
        int8_t gx = piece_x + PIECE_OFFSETS[current_piece_type][rot][i].dx - 1;
        int8_t gy = piece_y + PIECE_OFFSETS[current_piece_type][rot][i].dy;

        if (gx < 0 || gx >= BOARD_W || gy < 0 || gy >= BOARD_H)
            return 0;

        if (board[gy][gx] != 0)
            return 0;
    }
    return 1;
}

static uint8_t can_move_right(void)
{
    uint8_t i;
    for (i = 0; i < 4; i++)
    {
        int8_t gx = piece_x + PIECE_OFFSETS[current_piece_type][rot][i].dx + 1;
        int8_t gy = piece_y + PIECE_OFFSETS[current_piece_type][rot][i].dy;

        if (gx < 0 || gx >= BOARD_W || gy < 0 || gy >= BOARD_H)
            return 0;

        if (board[gy][gx] != 0)
            return 0;
    }
    return 1;
}

static uint8_t can_rotate_at_offset(uint8_t new_rot, int8_t x_offset)
{
    uint8_t i;
    for (i = 0; i < 4; i++)
    {
        int8_t gx = piece_x + x_offset + PIECE_OFFSETS[current_piece_type][new_rot][i].dx;
        int8_t gy = piece_y + PIECE_OFFSETS[current_piece_type][new_rot][i].dy;

        if (gx < 0 || gx >= BOARD_W || gy < 0 || gy >= BOARD_H)
            return 0;

        if (board[gy][gx] != 0)
            return 0;
    }
    return 1;
}

/* kept for completeness, not used directly now */
static uint8_t can_rotate(uint8_t new_rot)
{
    return can_rotate_at_offset(new_rot, 0);
}

/* ------------ Line clearing ------------ */
static uint8_t is_line_full(uint8_t row)
{
    uint8_t x;
    for (x = 0; x < BOARD_W; x++)
    {
        if (board[row][x] == 0)
            return 0;
    }
    return 1;
}

static void clear_lines(void)
{
    uint8_t y, x;
    int8_t src_y, dst_y;

    for (y = 0; y < BOARD_H; y++)
    {
        if (is_line_full(y))
        {
            for (dst_y = (int8_t)y; dst_y > 0; dst_y--)
            {
                src_y = (int8_t)(dst_y - 1);
                for (x = 0; x < BOARD_W; x++)
                {
                    board[dst_y][x] = board[src_y][x];
                }
            }
            for (x = 0; x < BOARD_W; x++)
            {
                board[0][x] = 0;
            }
            y--;
        }
    }
}

static void draw_board(void)
{
    uint8_t x, y;

    clear_screen();
    for (y = 0; y < BOARD_H; y++)
    {
        for (x = 0; x < BOARD_W; x++)
        {
            if (board[y][x])
            {
                draw_block_cell(x, y);
            }
        }
    }
}

/* ------------ 7-bag randomizer & preview ------------ */
static uint16_t rand_seed = 12345;

static uint16_t simple_rand(void)
{
    rand_seed = (uint16_t)((rand_seed * 1103515245u + 12345u) & 0x7FFFFFFFu);
    return (uint16_t)(rand_seed >> 16);
}

static void shuffle_bag(void)
{
    uint8_t i, j, temp;

    for (i = 0; i < 7; i++)
    {
        bag[i] = i;
    }

    for (i = 6; i > 0; i--)
    {
        j = (uint8_t)(simple_rand() % (i + 1));
        temp = bag[i];
        bag[i] = bag[j];
        bag[j] = temp;
    }

    bag_index = 0;
}

static uint8_t get_next_piece(void)
{
    if (bag_index >= 7)
    {
        shuffle_bag();
    }
    return bag[bag_index++];
}

static void update_preview(void)
{
    uint8_t saved_index = bag_index;
    uint8_t saved_bag[7];
    uint8_t i;

    for (i = 0; i < 7; i++)
    {
        saved_bag[i] = bag[i];
    }

    for (i = 0; i < 6; i++)
    {
        preview[i] = get_next_piece();
    }

    bag_index = saved_index;
    for (i = 0; i < 7; i++)
    {
        bag[i] = saved_bag[i];
    }
}

static void draw_preview(void)
{
    uint8_t i;
    uint8_t preview_x = 6;  /* right side columns */
    uint8_t preview_y;

    for (i = 0; i < 6; i++)
    {
        preview_y = (uint8_t)(i * 2 + 2);
        if (preview_y < BOARD_H)
        {
            draw_block_cell(preview_x, preview_y);
            if ((uint8_t)(preview_y + 1) < BOARD_H)
            {
                draw_block_cell(preview_x, (uint8_t)(preview_y + 1));
            }
        }
    }
}

/* ------------ main ------------ */
int main(void)
{
    uint8_t x, y;

    WDTCTL = WDTPW | WDTHOLD;

    init_clock();
    init_gpio();
    init_i2c();
    ssd_init();

    clear_screen();

    for (y = 0; y < BOARD_H; y++)
    {
        for (x = 0; x < BOARD_W; x++)
        {
            board[y][x] = 0;
        }
    }

    shuffle_bag();
    update_preview();
    current_piece_type = get_next_piece();

    piece_x = 3;
    piece_y = 0;
    rot     = 0;
    draw_preview();
    draw_piece();

    for (;;)
    {
        /* Rotate on P2.3 with wall kicks */
        if ((P2IN & BIT3) == 0)
        {
            uint8_t new_rot = (uint8_t)((rot + 1) & 3);

            if (can_rotate_at_offset(new_rot, 0))
            {
                clear_piece();
                rot = new_rot;
                draw_piece();
            }
            else if (can_rotate_at_offset(new_rot, 1))
            {
                clear_piece();
                piece_x++;
                rot = new_rot;
                draw_piece();
            }
            else if (can_rotate_at_offset(new_rot, -1))
            {
                clear_piece();
                piece_x--;
                rot = new_rot;
                draw_piece();
            }
            __delay_cycles(1600000);
        }

        /* Move left on P3.1 */
        if ((P3IN & BIT1) == 0)
        {
            if (can_move_left())
            {
                clear_piece();
                piece_x--;
                draw_piece();
            }
            __delay_cycles(1600000);
        }

        /* P1.1 right */
        if ((P1IN & BIT1) == 0)
        {
            if (can_move_right())
            {
                clear_piece();
                piece_x++;
                draw_piece();
            }
            __delay_cycles(1600000);
        }

        /* P1.2 left */
        if ((P1IN & BIT2) == 0)
        {
            if (can_move_left())
            {
                clear_piece();
                piece_x--;
                draw_piece();
            }
            __delay_cycles(1600000);
        }

        /* Auto drop */
        slow_divider++;
        if (slow_divider >= 10)
        {
            slow_divider = 0;
            drop_counter++;
        }

        if (drop_counter >= 5000)
        {
            drop_counter = 0;

            if (can_move_down())
            {
                clear_piece();
                piece_y++;
                draw_piece();
            }
            else
            {
                uint8_t i;
                uint8_t lines_were_cleared = 0;
                uint8_t old_count = 0;
                uint8_t new_count = 0;

                /* Count filled cells BEFORE locking (for cheap change detection) */
                for (y = 0; y < BOARD_H; y++)
                {
                    for (i = 0; i < BOARD_W; i++)
                    {
                        if (board[y][i]) old_count++;
                    }
                }

                /* Lock current piece */
                for (i = 0; i < 4; i++)
                {
                    int8_t gx = piece_x + PIECE_OFFSETS[current_piece_type][rot][i].dx;
                    int8_t gy = piece_y + PIECE_OFFSETS[current_piece_type][rot][i].dy;
                    if (gx >= 0 && gx < BOARD_W && gy >= 0 && gy < BOARD_H)
                    {
                        board[gy][gx] = 1;
                    }
                }

                /* Clear lines */
                clear_lines();

                /* Count filled cells AFTER */
                for (y = 0; y < BOARD_H; y++)
                {
                    for (i = 0; i < BOARD_W; i++)
                    {
                        if (board[y][i]) new_count++;
                    }
                }

                if (new_count < old_count)
                {
                    lines_were_cleared = 1;
                }

                if (lines_were_cleared)
                {
                    draw_board();
                }

                /* New piece from 7-bag */
                current_piece_type = get_next_piece();
                update_preview();
                piece_x = 3;
                piece_y = 0;
                rot     = 0;

                if (lines_were_cleared)
                {
                    draw_preview();
                }

                draw_piece();
            }
        }
    }
}
