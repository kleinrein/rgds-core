"""From a game's rating to the age it is for. launcher/age_rating.hpp holds the same table, and a test keeps them equal."""
# "Everyone" is every age: an owner's decision (docs/PROFILES_PLAN.md, section 12). Anything else, a board without a
# value included, is no rating: GameTDB lists thousands of those.
BOARDS = {
    'ESRB': {
        'EC': 3,
        'E': 3,
        'KA': 3,
        'E10+': 10,
        'T': 13,
        'M': 17,
        'AO': 18
    },
    'CERO': {
        'A': 3,
        'B': 12,
        'C': 15,
        'D': 17,
        'Z': 18
    },
    'GRB': {
        'ALL': 3,
        '12': 12,
        '15': 15,
        '18': 18
    },
    'CSRR': {
        'ALL': 3,
        '6': 6,
        '12': 12,
        '15': 15,
        '18': 18
    },
    'PEGI': {
        '3': 3,
        '4': 4,
        '6': 6,
        '7': 7,
        '12': 12,
        '16': 16,
        '18': 18
    },
    'USK': {
        '0': 3,
        '6': 6,
        '12': 12,
        '16': 16,
        '18': 18
    }
}


def minimum_age(rating):
    """'PEGI 12' -> 12; a board without a value ('ESRB'), 'ESRB NOT RATED' and anything unknown -> None."""
    board, _, value = (rating or '').strip().partition(' ')
    return BOARDS.get(board, {}).get(value.strip())
