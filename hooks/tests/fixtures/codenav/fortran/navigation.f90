MODULE Reads
  USE iso_fortran_env
  INCLUDE 'constants.inc'
  implicit none
  type :: BaseRead
    real :: quality
  end type BaseRead
  type, extends(BaseRead) :: Read
    integer :: length
  end type Read
contains
  SUBROUTINE Normalize(x)
    real, intent(inout) :: x
    CALL Helper(x)
    x = Score(x)
  end subroutine Normalize
  real FUNCTION Score(x) result(y)
    real, intent(in) :: x
    y = abs(x)
  end function Score
end module Reads
! subroutine phantom()
