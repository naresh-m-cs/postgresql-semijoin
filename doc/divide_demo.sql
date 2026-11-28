-- Load this file in psql, then run the example query below.
CREATE TEMP TABLE divide_enrollments (
    student_name text NOT NULL,
    course text NOT NULL
);

CREATE TEMP TABLE divide_required_courses (
    course text PRIMARY KEY
);

INSERT INTO divide_required_courses VALUES
    ('Database Systems'),
    ('Operating Systems');

INSERT INTO divide_enrollments VALUES
    ('Aditi Rao', 'Database Systems'),
    ('Aditi Rao', 'Operating Systems'),
    ('Aditi Rao', 'Computer Networks'),
    ('Kabir Shah', 'Database Systems'),
    ('Meera Nair', 'Operating Systems'),
    ('Rohan Das', 'Computer Networks'),
    ('Sara Khan', 'Database Systems'),
    ('Sara Khan', 'Operating Systems');

-- Which students took every required course?
-- SELECT student_name, course FROM divide_enrollments
-- DIVIDE BY
-- SELECT course FROM divide_required_courses;
--
-- Expected students: Aditi Rao and Sara Khan, in either order.
